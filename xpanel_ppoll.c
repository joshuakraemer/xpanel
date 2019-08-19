#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <xcb/xcb.h>
#include <xcb/xcb_event.h>
#include <string.h>

#ifdef __NetBSD__
#define ppoll pollts
#endif


/* used as return values for functions */
enum status {
	SUCCESS,
	FAILURE
};

struct timer {
	void (*callback)();
	void	*data;
};

xcb_screen_t *get_screen(xcb_connection_t *xcb_connection, int screen_number) {
	for (xcb_screen_iterator_t iterator = xcb_setup_roots_iterator(xcb_get_setup(xcb_connection));
			iterator.rem;
			--screen_number, xcb_screen_next(&iterator)) {
		if (screen_number == 0)
			return iterator.data;
	}
	return NULL;
}

void timer_callback(void *data) {
	puts("TIMER CALLBACK TRIGGERED");
}

void signal_handler(int signal, siginfo_t *info, void *context) {
	printf("Caught signal %d\n", signal);
	printf("value: %d\n", info->si_value.sival_int);
}

int main(int argc, char **argv) {
	int exit_code = EXIT_FAILURE;

	int screen_number;
	xcb_connection_t *xcb_connection = xcb_connect(NULL, &screen_number);
	if (xcb_connection_has_error(xcb_connection) > 0) {
		fputs("X connection has shut down due to a fatal error\n", stderr);
		goto out;
	}
	xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(xcb_connection)).data;

	/* used as index, keep synchronized with atom_names */
	enum {
		WM_DELETE_WINDOW,
		WM_PROTOCOLS,
		NET_ACTIVE_WINDOW,
		NET_CLIENT_LIST,
		NET_CURRENT_DESKTOP,
		NET_WM_NAME,
		NET_WM_WINDOW_TYPE,
		NET_WM_WINDOW_TYPE_DOCK,
		ATOM_COUNT
	};

	static char *atom_names[] = {
		"WM_DELETE_WINDOW",
		"WM_PROTOCOLS",
		"_NET_ACTIVE_WINDOW",
		"_NET_CLIENT_LIST",
		"_NET_CURRENT_DESKTOP",
		"_NET_WM_NAME",
		"_NET_WM_WINDOW_TYPE",
		"_NET_WM_WINDOW_TYPE_DOCK",
	};

	xcb_intern_atom_cookie_t atom_cookies[ATOM_COUNT];
	for (int i = 0; i < ATOM_COUNT; ++i)
		atom_cookies[i] = xcb_intern_atom(xcb_connection, 0, strlen(atom_names[i]), atom_names[i]);

	xcb_intern_atom_reply_t *atom_reply;
	xcb_generic_error_t *error;
	xcb_atom_t atoms[ATOM_COUNT];
	for (int i = 0; i < ATOM_COUNT; ++i) {
		if ((atom_reply = xcb_intern_atom_reply(xcb_connection, atom_cookies[i], &error)) == NULL) {
			fputs("Could not get atom\n", stderr);
			free(error);
		} else {
			atoms[i] = atom_reply->atom;
			free(atom_reply);
		}
	}

	int xcb_fd = xcb_get_file_descriptor(xcb_connection);

	xcb_window_t window = xcb_generate_id(xcb_connection);
	uint32_t mask = XCB_CW_BACK_PIXEL|XCB_CW_EVENT_MASK;
	uint32_t values[] = {
		screen->white_pixel,
		XCB_EVENT_MASK_EXPOSURE|XCB_EVENT_MASK_BUTTON_PRESS
	};

	xcb_create_window(xcb_connection, XCB_COPY_FROM_PARENT, window, screen->root, 0, screen->height_in_pixels - 48, screen->width_in_pixels, 48, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);
	xcb_change_property(xcb_connection, XCB_PROP_MODE_REPLACE, window, atoms[NET_WM_WINDOW_TYPE], XCB_ATOM_ATOM, 32, 1, &atoms[NET_WM_WINDOW_TYPE_DOCK]);
	xcb_change_property(xcb_connection, XCB_PROP_MODE_REPLACE, window, atoms[WM_PROTOCOLS], XCB_ATOM_ATOM, 32, 1, &atoms[WM_DELETE_WINDOW]);

	xcb_map_window(xcb_connection, window);

	xcb_flush(xcb_connection);

	/*
	struct sigaction action_settings = {
		.sa_sigaction = &signal_handler,
		.sa_flags = SA_SIGINFO
	};
	sigemptyset(&action_settings.sa_mask);
	if (sigaction(SIGALRM, &action_settings, NULL) == -1) {
		perror("sigaction");
		goto out;
	}
	*/

	/*struct timer test_timer = {
		.callback = &timer_callback,
		.data = NULL
	};*/

	struct sigevent signal_settings = {
		.sigev_notify = SIGEV_SIGNAL,
		.sigev_signo = SIGALRM,
		//.sigev_value = {.sival_ptr = &test_timer}
		.sigev_value.sival_int = 12
	};
	timer_t timer;
	if (timer_create(CLOCK_REALTIME, &signal_settings, &timer) == -1) {
		perror("timer_create");
	}
	struct itimerspec timer_settings = {
		.it_interval = {0, 0},
		.it_value = {3, 0}
	};
	if (timer_settime(timer, 0, &timer_settings, NULL) == -1) {
		perror("timer_settime");
	}


	struct timespec timeout = {10, 0};
	sigset_t signal_mask;
	sigemptyset(&signal_mask);
	sigaddset(&signal_mask, SIGALRM);
	int ready = ppoll(NULL, 0, NULL, &signal_mask);
	printf("READY? %d\n", ready);


	xcb_generic_event_t *event;
	while ((event = xcb_wait_for_event(xcb_connection)) != NULL) {
		if (event->response_type == 0) {
			fprintf(stderr, "Error received: %s\n", xcb_event_get_error_label(XCB_EVENT_RESPONSE_TYPE(event)));
			free(event);
			continue;
		}

		printf("Event received: %s\n", xcb_event_get_label(XCB_EVENT_RESPONSE_TYPE(event)));

		switch (XCB_EVENT_RESPONSE_TYPE(event)) {
			case XCB_EXPOSE:
				break;
			case XCB_CLIENT_MESSAGE:
				if((*(xcb_client_message_event_t *) event).data.data32[0] == atoms[WM_DELETE_WINDOW])
					exit(EXIT_SUCCESS);
				break;
		}
	}

	exit_code = EXIT_SUCCESS;

	out:
		xcb_disconnect(xcb_connection);

		return exit_code;
}
