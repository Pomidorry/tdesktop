/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/export_manager.h"

#include "export/export_controller.h"
#include "export/export_settings.h"
#include "export/output/export_output_abstract.h"
#include "export/view/export_view_panel_controller.h"
#include "data/data_peer.h"
#include "main/main_session.h"
#include "main/main_account.h"
#include "storage/storage_account.h"
#include "ui/layers/box_content.h"
#include "base/unixtime.h"
#include <QtCore/QSet>
#include <QtCore/QSaveFile>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QFileInfo>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QDateTime>

namespace Export {


namespace {

QString ExtractTextFromExportMessage(const QJsonValue &value) {
	if (value.isString()) {
		return value.toString();
	} else if (value.isObject()) {
		const auto object = value.toObject();
		if (const auto i = object.constFind("text"); i != object.constEnd()) {
			return ExtractTextFromExportMessage(*i);
		}
		return QString();
	} else if (value.isArray()) {
		auto result = QString();
		for (const auto &part : value.toArray()) {
			result += ExtractTextFromExportMessage(part);
		}
		return result;
	}
	return QString();
}

QString ChatFileName(const QJsonObject &chat, QSet<QString> &used) {
	auto base = chat.value("name").toString();
	if (base.isEmpty()) {
		base = chat.value("id").toString();
	}
	if (base.isEmpty()) {
		base = "chat";
	}
	for (auto &ch : base) {
		if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?'
				|| ch == '"' || ch == '<' || ch == '>' || ch == '|') {
			ch = '_';
		}
	}
	auto result = base;
	auto index = 2;
	while (used.contains(result)) {
		result = base + "_" + QString::number(index++);
	}
	used.insert(result);
	return result + ".txt";
}

void WriteChatTextFiles(const QString &mainFilePath) {
	auto input = QFile(mainFilePath);
	if (!input.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto parsed = QJsonDocument::fromJson(input.readAll());
	if (!parsed.isObject()) {
		return;
	}
	const auto root = parsed.object();
	auto dir = QDir(QFileInfo(mainFilePath).absolutePath());
	if (!dir.mkpath("chats_text")) {
		return;
	}
	dir.cd("chats_text");
	auto used = QSet<QString>();
	const auto processSection = [&](const char *name) {
		const auto section = root.value(name).toObject();
		const auto list = section.value("list").toArray();
		for (const auto &chatValue : list) {
			const auto chat = chatValue.toObject();
			auto output = QSaveFile(dir.filePath(ChatFileName(chat, used)));
			if (!output.open(QIODevice::WriteOnly)) {
				continue;
			}
			const auto messages = chat.value("messages").toArray();
			for (const auto &messageValue : messages) {
				const auto message = messageValue.toObject();
				auto text = ExtractTextFromExportMessage(message.value("text"));
				text = text.trimmed();
				if (text.isEmpty()) {
					continue;
				}
				text.replace('\n', ' ');
				const auto date = message.value("date").toString();
				const auto line = (date.isEmpty() ? text : (date + "\t" + text)) + "\n";
				output.write(line.toUtf8());
			}
			output.commit();
		}
	};
	processSection("chats");
	processSection("left_chats");
}

} // namespace

void WriteChatTextFilesFromJson(const QString &mainFilePath) {
	WriteChatTextFiles(mainFilePath);
}

Manager::Manager() = default;

Manager::~Manager() = default;

void Manager::start(not_null<PeerData*> peer) {
	start(&peer->session(), peer->input());
}

void Manager::startTopic(
		not_null<PeerData*> peer,
		MsgId topicRootId,
		const QString &topicTitle) {
	if (_panel) {
		_panel->activatePanel();
		return;
	}
	_controller = std::make_unique<Controller>(
		&peer->session().mtp(),
		peer->input(),
		int32(topicRootId.bare),
		uint64(peer->id.value),
		topicTitle);
	setupPanel(&peer->session());
}

void Manager::start(
		not_null<Main::Session*> session,
		const MTPInputPeer &singlePeer) {
	if (_panel) {
		_panel->activatePanel();
		return;
	}
	_controller = std::make_unique<Controller>(
		&session->mtp(),
		singlePeer);
	setupPanel(session);
}

void Manager::startAllChatsJsonBackground(not_null<Main::Session*> session) {
	if (_controller) {
		if (_panel) {
			_panel->activatePanel();
		}
		return;
	}
	_controller = std::make_unique<Controller>(
		&session->mtp(),
		MTP_inputPeerEmpty());
	auto settings = session->local().readExportSettings();
	settings.singlePeer = MTP_inputPeerEmpty();
	settings.singlePeerFrom = base::unixtime::serialize(
		QDateTime::currentDateTime().addMonths(-12));
	settings.singlePeerTill = 0;
	settings.singleTopicRootId = 0;
	settings.singleTopicPeerId = 0;
	settings.singleTopicTitle = QString();
	settings.types = Settings::Type::AnyChatsMask;
	settings.fullChats = Settings::Type::AnyChatsMask;
	settings.media.types = MediaSettings::Types();
	settings.format = Output::Format::Json;
	View::ResolveSettings(session, settings);
	_backgroundLifetime = rpl::lifetime();
	session->account().sessionChanges(
	) | rpl::filter([=](Main::Session *value) {
		return (value != session);
	}) | rpl::on_next([=] {
		stop();
	}, _backgroundLifetime);
	_controller->state(
	) | rpl::on_next([=](State &&state) {
		if (const auto finished = std::get_if<FinishedState>(&state)) {
			const auto mainFilePath = finished->path;
			LOG(("Export Info: Finished background all chats JSON export: %1.")
				.arg(mainFilePath));
			stop();
			crl::async([mainFilePath] {
				WriteChatTextFilesFromJson(mainFilePath);
			});
		} else if (const auto error = std::get_if<ApiErrorState>(&state)) {
			LOG(("Export Info: Background all chats JSON export API Error '%1'.")
				.arg(error->data.type()));
			stop();
		} else if (const auto error = std::get_if<OutputErrorState>(&state)) {
			LOG(("Export Info: Background all chats JSON export Disk Error '%1'.")
				.arg(error->path));
			stop();
		} else if (v::is<CancelledState>(state)) {
			LOG(("Export Info: Background all chats JSON export cancelled."));
			stop();
		}
	}, _backgroundLifetime);
	_controller->startExport(settings, View::PrepareEnvironment(session), true);
}

void Manager::setupPanel(not_null<Main::Session*> session) {
	_panel = std::make_unique<View::PanelController>(
		session,
		_controller.get());
	session->account().sessionChanges(
	) | rpl::filter([=](Main::Session *value) {
		return (value != session);
	}) | rpl::on_next([=] {
		stop();
	}, _panel->lifetime());

	_viewChanges.fire(_panel.get());

	_panel->stopRequests(
	) | rpl::on_next([=] {
		LOG(("Export Info: Stop requested."));
		stop();
	}, _controller->lifetime());
}

rpl::producer<View::PanelController*> Manager::currentView(
) const {
	return _viewChanges.events_starting_with(_panel.get());
}

bool Manager::inProgress() const {
	return _controller != nullptr;
}

bool Manager::inProgress(not_null<Main::Session*> session) const {
	return _panel && (&_panel->session() == session);
}

void Manager::stopWithConfirmation(Fn<void()> callback) {
	if (!_panel) {
		callback();
		return;
	}
	auto closeAndCall = [=, callback = std::move(callback)]() mutable {
		auto saved = std::move(callback);
		LOG(("Export Info: Stop With Confirmation."));
		stop();
		if (saved) {
			saved();
		}
	};
	_panel->stopWithConfirmation(std::move(closeAndCall));
}

void Manager::stop() {
	if (_panel) {
		LOG(("Export Info: Destroying."));
		_panel = nullptr;
		_viewChanges.fire(nullptr);
	}
	_controller = nullptr;
	_backgroundLifetime = rpl::lifetime();
}

} // namespace Export
