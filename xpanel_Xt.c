#include <stdio.h>
#include <stdlib.h>
#include <locale.h>
#include <X11/Intrinsic.h>
#include <X11/Shell.h>
#include <X11/StringDefs.h>
#include <X11/Xaw/Box.h>
#include <X11/Xaw/Form.h>
#include <X11/Xaw/Command.h>
#include <X11/Xaw/SimpleMenu.h>
#include <X11/Xaw/SmeBSB.h>
#include <X11/Xaw/MenuButton.h>
#include <X11/Xatom.h>
//#include "xpanel.h"

#include "xclock/Clock.h"
Boolean no_locale = False;

#include <unistd.h>
#include <x86intrin.h>


struct timer {
	unsigned long long	start;
	unsigned long long	sum;
	unsigned long long	last_duration;
	unsigned long long	count;
};

struct timer find_window_timer, find_app_timer, create_app_timer, create_client_timer, append_app_timer, append_client_timer, remove_app_timer, remove_client_timer, update_client_list_timer = {0};

void start_timer(struct timer *timer) {
	timer->start = __rdtsc();
}

void stop_timer(struct timer *timer) {
	unsigned long long stop = __rdtsc();

	if (timer->start == 0) return;

	timer->last_duration = stop - timer->start;
	timer->sum += timer->last_duration;
	timer->count++;
	timer->start = 0;
}

void show_timer(struct timer *timer, char *name) {
	puts("──────────────────────────────────────────────────────────────────────");
	if (timer->count)
		printf("%s\t%'6lld  %'14lld  %'14lld  %'14lld\n", name, timer->count, timer->last_duration, timer->sum/timer->count, timer->sum);
}

void show_timers() {
	puts("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
	puts("timer            count   cycles (last)   cycles (mean)   cycles(total)");
	show_timer(&find_window_timer, "find_window");
	show_timer(&find_app_timer, "find_app");
	show_timer(&create_app_timer, "create_app");
	show_timer(&create_client_timer, "create_client");
	show_timer(&append_app_timer, "append_app");
	show_timer(&append_client_timer, "append_client");
	show_timer(&remove_app_timer, "remove_app");
	show_timer(&remove_client_timer, "remove_client");
	show_timer(&update_client_list_timer, "update_client_l");
	puts("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
}


/* used as return values for functions */
enum status {
	SUCCESS,
	FAILURE
};

/* keep synchronized with atom_names */
enum {
	WM_DELETE_WINDOW,
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
	"_NET_ACTIVE_WINDOW",
	"_NET_CLIENT_LIST",
	"_NET_CURRENT_DESKTOP",
	"_NET_WM_NAME",
	"_NET_WM_WINDOW_TYPE",
	"_NET_WM_WINDOW_TYPE_DOCK",
};

struct server {
	Display	*display;
	XtAppContext	app_context;
	Widget	app_shell;
	Window	root_window;
	Atom	atoms[ATOM_COUNT];
};
static struct server server;

struct taskbar {
	Widget	app_box;
	Widget	client_event_receiver;
	struct app	*first_app;
	struct app	*last_app;
};
static struct taskbar taskbar;

struct app {
	struct app	*next_app;
	char	*class;
	struct client	*first_client;
	struct client	*last_client;
	Widget	button;
	Widget	menu;
	Bool	focused;
};

/* clients = application windows managed by the window manager */
struct client {
	struct client	*next_client;
	Window	window;
	Widget	menu_item;
	Bool	focused;
};


/* https://stackoverflow.com/a/27832746 */
void utf8_copy(char *source, char *destination, size_t destination_size) {
	if (destination_size == 0)
		return;

	size_t copy_size = strlen(source); // does not include NULL

	while (copy_size >= destination_size) {
		char *last_byte = source + destination_size;
		while (last_byte > source) {
			last_byte--;
			if ((*last_byte & 0xC0) != 0x80) // initial byte of potentially multi-byte character or NULL
				break;
		}
		copy_size = last_byte - source;
	}
	memcpy(destination, source, copy_size);
	destination[copy_size] = '\0';
}

/* gets the list of currently open client windows, i.e. application windows managed by the window manager */
/* window_list must be freed with XFree */
enum status get_window_list(Window **window_list, long unsigned int *window_count) {
	Atom actual_type;
	int item_size;
	unsigned long leftover_byte_count;

	/* max. size: 256 x 4-byte multiples = 1 KiB */
	if (XGetWindowProperty(server.display, server.root_window, server.atoms[NET_CLIENT_LIST], 0L, 256L, False, XA_WINDOW, &actual_type, &item_size, window_count, &leftover_byte_count, (unsigned char **) window_list) != Success || actual_type == None) {
		fputs("Could not get NET_CLIENT_LIST property\n", stderr);
		return FAILURE;
	}

	return SUCCESS;
}

enum status get_window_property(Window window, Atom property) {
	Atom requested_type = AnyPropertyType;
	Atom actual_type;
	int item_size;
	unsigned long returned_item_count;
	unsigned long leftover_byte_count;
	unsigned char *data;

	Status status = XGetWindowProperty(server.display, window, property, 0L, 1024L, False, requested_type, &actual_type, &item_size, &returned_item_count, &leftover_byte_count, &data);

	if (actual_type == None)
		puts("property does not exist");

	if (actual_type != requested_type)
		printf("property has different type (%s)\n", XGetAtomName(server.display, actual_type));

	if (status != Success)
		puts("failure");

	return SUCCESS;
}

enum status get_window_title(Window window, char *title_return, int buffer_size) {
	enum status return_code = FAILURE;

	XTextProperty text_property;
	char **text_list = NULL;

	if (XGetTextProperty(server.display, window, &text_property, server.atoms[NET_WM_NAME]) == 0) {
		if (XGetWMName(server.display, window, &text_property) == 0) {
			fputs("Could not get window title property\n", stderr);
			goto out;
		}
	}

	/* the text property is converted to a text list to convert it to the encoding of the current locale */
	int string_count = 0;
	XmbTextPropertyToTextList(server.display, &text_property, &text_list, &string_count);

	size_t string_size = strlen(text_list[0]) + 1; // including NULL
	size_t copy_size = string_size < buffer_size ? string_size : buffer_size;
	memcpy(title_return, text_list[0], copy_size);

	return_code = SUCCESS;

	out:
		XFree(text_property.value);
		XFreeStringList(text_list);
		return return_code;
}

/*
char title[TITLE_LENGTH + 1];
title[0] = 0;

if (get_window_title(window, title, TITLE_LENGTH + 1) != SUCCESS)
	fputs("Could not get window title\n", stderr);
*/

enum status get_window_class(Window window, char **return_class) {
	XClassHint class_hint = {.res_name = NULL, .res_class = NULL};
	if (XGetClassHint(server.display, window, &class_hint) == 0) {
		XFree(class_hint.res_name);
		XFree(class_hint.res_class);
		return FAILURE;
	}
	XFree(class_hint.res_name);

	*return_class = class_hint.res_class;
	return SUCCESS;
}

void raise_window(Window window) {
	XRaiseWindow(server.display, window);
	XSetInputFocus(server.display, window, RevertToParent, CurrentTime);
}

void print_clients(struct app *app) {
	printf("%s:\n", app->class);
	struct client *current_client = app->first_client;
	while (current_client != NULL) {
		printf("\t– ...\n");
		current_client = current_client->next_client;
	}
}

void button_callback(Widget button, XtPointer client_data, XtPointer call_data) {
	show_timers();
}

void menu_callback(Widget menu_item, XtPointer client_data, XtPointer call_data) {
	raise_window(*((Window *) client_data));
	//show_timers();
}

enum status create_app(struct taskbar *taskbar, char *class, struct app **return_app) {
	start_timer(&create_app_timer);

	struct app *app = malloc(sizeof *app);
	if (app == NULL) {
		perror("malloc");
		stop_timer(&create_app_timer);
		return FAILURE;
	}

	app->next_app = NULL;
	app->class = class;
	app->first_client = app->last_client = NULL;
	//app->button = XtVaCreateManagedWidget(NULL, commandWidgetClass, taskbar->app_box, XtNlabel, app->class, NULL);
	//XtAddCallback(app->button, XtNcallback, button_callback, (XtPointer)app);
	app->button = XtVaCreateManagedWidget(NULL, menuButtonWidgetClass, taskbar->app_box, XtNlabel, app->class, NULL);
	app->menu = XtVaCreatePopupShell("menu", simpleMenuWidgetClass, app->button, NULL);



	*return_app = app;
	stop_timer(&create_app_timer);
	return SUCCESS;
}

enum status create_client(struct taskbar *taskbar, struct app *app, Window window, struct client **return_client) {
	start_timer(&create_client_timer);

	struct client *client = malloc(sizeof *client);
	if (client == NULL) {
		perror("malloc");
		stop_timer(&create_client_timer);
		return FAILURE;
	}

	client->next_client = NULL;
	client->window = window;

	char title[128] = "[unknown title]";
	if (get_window_title(window, title, 128) != SUCCESS)
		fputs("Could not get window title\n", stderr);

	printf("title '%s'\n", title);

	client->menu_item = XtVaCreateManagedWidget(NULL, smeBSBObjectClass, app->menu, XtNlabel, title, NULL);
	XtAddCallback(client->menu_item, XtNcallback, menu_callback, &client->window);

	/* listen for DestroyNotify event on window */
	XtRegisterDrawable(server.display, window, taskbar->client_event_receiver);
	XSelectInput(server.display, window, StructureNotifyMask|PropertyChangeMask);

	*return_client = client;
	stop_timer(&create_client_timer);
	return SUCCESS;
}

void append_app(struct taskbar *taskbar, struct app *app) {
	start_timer(&append_app_timer);

	app->next_app = NULL;

	if (taskbar->first_app == NULL) {
		taskbar->first_app = taskbar->last_app = app;
	} else {
		taskbar->last_app->next_app = app;
		taskbar->last_app = app;
	}

	stop_timer(&append_app_timer);
}

void append_client(struct app *app, struct client *client) {
	start_timer(&append_client_timer);

	client->next_client = NULL;

	if (app->first_client == NULL) {
		app->first_client = app->last_client = client;
	} else {
		app->last_client->next_client = client;
		app->last_client = client;
	}

	stop_timer(&append_client_timer);
}

Bool find_window(struct taskbar *taskbar, Window window, struct app **return_previous_app, struct client **return_previous_client) {
	//start_timer(&find_window_timer);

	struct app *current_app = taskbar->first_app;
	struct app *previous_app = NULL;

	while (current_app != NULL) {
		//printf("\tcurr. app: %s\n", current_app->class);
		struct client *current_client = current_app->first_client;
		struct client *previous_client = NULL;

		while (current_client != NULL) {
			if (current_client->window == window) {
				if (return_previous_app)	*return_previous_app = previous_app;
				if (return_previous_client)	*return_previous_client = previous_client;
				stop_timer(&find_window_timer);
				return True;
			}
			previous_client = current_client;
			current_client = current_client->next_client;
		}
		previous_app = current_app;
		current_app = current_app->next_app;
	}
	//puts("\t...not found");

	//stop_timer(&find_window_timer);
	return False;
}

Bool find_app(struct taskbar *taskbar, char *class, struct app **return_app) {
	start_timer(&find_app_timer);
	struct app *current_app = taskbar->first_app;

	while (current_app != NULL) {
		//printf("\tcurr. app: %s\n", current_app->class);
		if (strcmp(current_app->class, class) == 0) {
			if (return_app) *return_app = current_app;

			stop_timer(&find_app_timer);
			return True;
		}
		current_app = current_app->next_app;
	}

	stop_timer(&find_app_timer);
	*return_app = NULL;
	return False;
}

void remove_app(struct taskbar *taskbar, struct app *previous_app) {
	start_timer(&remove_app_timer);

	struct app *app = NULL;
	if (previous_app) {
		app = previous_app->next_app;
	} else {
		app = taskbar->first_app;
	}

	if (taskbar->first_app == app) {
		taskbar->first_app = app->next_app;
	} else {
		previous_app->next_app = app->next_app;
	}

	if (taskbar->last_app == app) {
		taskbar->last_app = previous_app;
	}

	XtDestroyWidget(app->button);
	XFree(app->class);
	free(app);

	stop_timer(&remove_app_timer);
}

void remove_client(struct taskbar *taskbar, struct app *previous_app, struct client *previous_client) {
	start_timer(&remove_client_timer);

	struct app *app = NULL;
	if (previous_app) {
		app = previous_app->next_app;
	} else {
		app = taskbar->first_app;
	}

	struct client *client;
	if (previous_client) {
		client = previous_client->next_client;
	} else {
		client = app->first_client;
	}

	if (app->first_client == app->last_client) {
		app->first_client = app->last_client = NULL;
		remove_app(taskbar, previous_app);
	} else {
		if (app->first_client == client) {
			app->first_client = client->next_client;
		} else {
			previous_client->next_client = client->next_client;
		}

		if (app->last_client == client) {
			app->last_client = previous_client;
		}
	}

	free(client);

	stop_timer(&remove_client_timer);
}

enum status update_client_list(struct taskbar *taskbar) {
	start_timer(&update_client_list_timer);
	enum status return_code = FAILURE;

	Window *window_list;
	long unsigned int window_list_size;
	if (get_window_list(&window_list, &window_list_size) == FAILURE) {
		fputs("Could not get window list\n", stderr);
		goto out;
	}

	for (int i = 0; i < window_list_size; i++) {
		if (find_window(taskbar, window_list[i], NULL, NULL) == True) {
			continue;
		}

		char *class = NULL;
		if (get_window_class(window_list[i], &class) == FAILURE) {
			/* the window may have been destroyed or may lack the WM_CLASS property */
			fputs("Could not get window class\n", stderr);
			XFree(class);
			continue;
		}

		if (strcmp(class, "XPanel") == 0) {
			XFree(class);
			continue;
		}

		/* Window focused_window = 0;
		int focus_revert;
		XGetInputFocus(server.display, &focused_window, &focus_revert); */

		struct app *app = NULL;
		if (find_app(taskbar, class, &app) == False) {
			create_app(taskbar, class, &app);
			append_app(taskbar, app);
		}

		struct client *new_client = NULL;
		if (create_client(taskbar, app, window_list[i], &new_client) == FAILURE) {
			fputs("Could not create new client\n", stderr);
			goto out;
		}

		append_client(app, new_client);
	}

	return_code = SUCCESS;

	out:
		XFree(window_list);
		stop_timer(&update_client_list_timer);

		return return_code;
}

int handle_error(Display *display, XErrorEvent *error_event) {
	char error_message[256];
	XGetErrorText(server.display, error_event->error_code, error_message, 256);
	fprintf(stderr, "X error: %s\n", error_message);
	return 0;
}

void handle_own_events(Widget widget, XtPointer client_data, XEvent *event, Boolean *continue_dispatch) {
	if (event->type == ClientMessage && event->xclient.data.l[0] == server.atoms[WM_DELETE_WINDOW]) {
		exit(EXIT_SUCCESS);
	}
}

void handle_root_events(Widget widget, XtPointer client_data, XEvent *event, Boolean *continue_dispatch) {
	if (event->xproperty.atom == server.atoms[NET_ACTIVE_WINDOW]) {
		//puts("active window changed");
	} else if (event->xproperty.atom == server.atoms[NET_CURRENT_DESKTOP]) {
		//puts("current desktop changed");
	} else if (event->xproperty.atom == server.atoms[NET_CLIENT_LIST]) {
		update_client_list(&taskbar);
	}
}

void handle_client_events(Widget widget, XtPointer client_data, XEvent *event, Boolean *continue_dispatch) {
	if (event->type == DestroyNotify) {
		struct app *previous_app = NULL;
		struct client *previous_client = NULL;
		if (find_window(&taskbar, event->xproperty.window, &previous_app, &previous_client) == True) {
			remove_client(&taskbar, previous_app, previous_client);
		}
	} else if (event->type == PropertyNotify) {
		if ((event->xproperty.atom == XA_WM_NAME) || (event->xproperty.atom == server.atoms[NET_WM_NAME])) {
			/* puts("Title changed"); */
		}
	}
}

void test_callback(Widget widget, XtPointer client_data, XtPointer call_data) {
	show_timers();
}


int main(int argc, char **argv) {
	setlocale (LC_ALL, "");

	XSetErrorHandler(&handle_error);

	String fallback_resources[] = {
		//"XPanel.height: 50",
		"*startButton.foreground: rgb:00/99/99",
		"*startButton.borderColor: rgb:00/99/99",
		//"*MenuButton.height: 24",
		"*borderWidth: 2",
		"*form.defaultDistance: 6",
		"*appBox.borderWidth: 0",
		"*appBox.hSpace: 6",
		"*appBox.vSpace: 0",
		"*Clock.analog: False",
		"*Clock.borderColor: rgb:7f/7f/7f",
		"*Clock.face: mono-10",
		"*Clock.padding: 3",
		"*Clock.strftime: %F %R",
		NULL
	};
	server.app_shell = XtVaOpenApplication(&server.app_context, "XPanel", NULL, 0, &argc, argv, fallback_resources, applicationShellWidgetClass, NULL);
	server.display = XtDisplay(server.app_shell);
	Screen *screen = DefaultScreenOfDisplay(server.display);
	server.root_window = RootWindowOfScreen(screen);

	XInternAtoms(server.display, atom_names, ATOM_COUNT, False, server.atoms);

	Widget form = XtVaCreateManagedWidget("form", formWidgetClass, server.app_shell, NULL);
	Widget start_button = XtVaCreateManagedWidget("startButton", menuButtonWidgetClass, form, XtNlabel, "Menu", XtNleft, XawChainLeft, XtNright, XawChainLeft, XtNtop, XawChainTop, XtNbottom, XawChainBottom, NULL);
	Dimension button_height, button_border_width;
	XtVaGetValues(start_button, XtNheight, &button_height, XtNborderWidth, &button_border_width, NULL);

	taskbar.app_box = XtVaCreateManagedWidget("appBox", boxWidgetClass, form, XtNheight, button_height + 2*button_border_width, XtNfromHoriz, start_button, XtNleft, XawChainLeft, XtNright, XawChainRight, XtNtop, XawChainTop, XtNbottom, XawChainBottom, NULL);
	Widget clock = XtVaCreateManagedWidget("clock", clockWidgetClass, form, XtNfromHoriz, taskbar.app_box, XtNleft, XawChainRight, XtNright, XawChainRight, XtNtop, XawChainTop, XtNbottom, XawChainBottom, NULL);

	XtRealizeWidget(server.app_shell);

	int screen_width = WidthOfScreen(screen);
	int screen_height = HeightOfScreen(screen);
	Dimension shell_height;
	XtVaGetValues(server.app_shell, XtNheight, &shell_height, NULL);
	XtVaSetValues(server.app_shell, XtNx, 0, XtNy, screen_height - shell_height, XtNwidth, screen_width, NULL);

	Window main_window = XtWindow(server.app_shell);
	XChangeProperty(server.display, main_window, server.atoms[NET_WM_WINDOW_TYPE], XA_ATOM, 32, PropModeReplace, (unsigned char *) &server.atoms[NET_WM_WINDOW_TYPE_DOCK], 1);
	XSetWMProtocols(server.display, main_window, &server.atoms[WM_DELETE_WINDOW], 1);


	/* handle ClientMessage events on our own window (_WM_DELETE_WINDOW) */
	XtAddEventHandler(server.app_shell, NoEventMask, True, &handle_own_events, NULL);

	/* events on other windows will be dispatched to those widgets */
	Widget root_event_receiver = XtVaCreateWidget("root_event_receiver", coreWidgetClass, server.app_shell, NULL);
	taskbar.client_event_receiver = XtVaCreateWidget("client_event_receiver", coreWidgetClass, server.app_shell, NULL);

	/* listen for events on the root window */
	XtRegisterDrawable(server.display, server.root_window, root_event_receiver);
	XSelectInput(server.display, server.root_window, PropertyChangeMask);
	XtAddRawEventHandler(root_event_receiver, PropertyChangeMask, False, &handle_root_events, NULL);

	/* handle events on client windows */
	XtAddRawEventHandler(taskbar.client_event_receiver, StructureNotifyMask|PropertyChangeMask, False, &handle_client_events, NULL);


	taskbar.first_app = taskbar.last_app = NULL;


	XtAppMainLoop(server.app_context);
	return EXIT_SUCCESS;
}
