// Two windows, one shared X11 connection, and the plugin pump that drains it.
//
// THE DEFECT THIS PINS: `WindowX11::processPluginFdEvents` calls XNextEvent on the PROCESS-GLOBAL
// display, so it takes every queued event off the shared connection — including events belonging to
// other windows. Before the lookup fallback existed those were read and DROPPED, so a plugin that
// opened a second window found it drew once and then went deaf.
//
// THE ASSERTION IS A COUNT ON THE SECOND WINDOW'S OWN DRAW CALLBACK, not a repaint or a pixel: the
// claim is "the event reached the window it was addressed to", and that is a value, so it is read as
// one.

#include "visage_windowing/linux/windowing_x11.h"
#include "visage_windowing/windowing.h"

#include <X11/Xlib.h>
#include <cstdio>
#include <memory>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
	std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		++failures;
	}
}

/// Post a timer ClientMessage to `handle` — the same message visage's own timer thread sends, which
/// is what a window's draw request looks like on this backend.
void postTimerEvent(::Window handle) {
	visage::X11Connection* x11 = visage::X11Connection::globalInstance();
	XClientMessageEvent event {};
	event.type = ClientMessage;
	event.window = handle;
	event.message_type = x11->timerEvent();
	event.format = 32;
	XSendEvent(x11->display(), handle, False, 0, reinterpret_cast<XEvent*>(&event));
	// SYNC, NOT FLUSH. XFlush only pushes our request out; the event does not appear in OUR queue
	// until the server has processed it and sent it back, so a pump run straight after a flush finds
	// XPending == 0 and drains nothing. That is a bug in the TEST, not the pump, and it made the
	// first draft report failures for behaviour that had never changed.
	XSync(x11->display(), False);
}

} // namespace

int main() {
	auto first = visage::createWindow(visage::Dimension(200), visage::Dimension(120));
	auto second = visage::createWindow(visage::Dimension(200), visage::Dimension(120));
	if (first == nullptr || second == nullptr) {
		std::printf("FAIL  could not create two windows\n");
		return 1;
	}
	first->show();
	second->show();

	int firstDraws = 0;
	int secondDraws = 0;
	first->setDrawCallback([&](double) { ++firstDraws; });
	second->setDrawCallback([&](double) { ++secondDraws; });

	// THE EVENT IS ADDRESSED TO THE SECOND WINDOW AND PUMPED BY THE FIRST. That is precisely the
	// arrangement a plugin is in: the editor's pump is the only one running.
	postTimerEvent(static_cast<::Window>(reinterpret_cast<uintptr_t>(second->nativeHandle())));
	first->processPluginFdEvents();

	check(secondDraws > 0, "an event addressed to the second window reaches it");

	// AND THE FIRST WINDOW IS NOT DRAWN BY IT — or the assertion above could be satisfied by a pump
	// that broadcasts every event to every window, which is a different bug with the same symptom.
	const int firstAfter = firstDraws;
	postTimerEvent(static_cast<::Window>(reinterpret_cast<uintptr_t>(second->nativeHandle())));
	first->processPluginFdEvents();
	check(firstDraws == firstAfter, "and it is not delivered to the pumping window as well");

	// THE PUMPING WINDOW STILL RECEIVES ITS OWN, which is the behaviour that existed before and must
	// not have been traded away for the fix.
	postTimerEvent(static_cast<::Window>(reinterpret_cast<uintptr_t>(first->nativeHandle())));
	first->processPluginFdEvents();
	check(firstDraws > firstAfter, "the pumping window still receives its own events");

	std::printf("%s\n", failures == 0 ? "SUCCESS" : "FAILURE");
	return failures == 0 ? 0 : 1;
}
