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
#include "ui/layers/box_content.h"
#include "base/unixtime.h"

namespace Export {

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
	settings.singlePeerFrom = 0;
	settings.singlePeerTill = 0;
	settings.singleTopicRootId = 0;
	settings.singleTopicPeerId = 0;
	settings.singleTopicTitle = QString();
	settings.types = Settings::Type::AnyChatsMask;
	settings.fullChats = Settings::Type::AnyChatsMask;
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
			LOG(("Export Info: Finished background all chats JSON export: %1.")
				.arg(finished->path));
			stop();
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
