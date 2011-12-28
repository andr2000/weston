/*
 * Copyright © 2011 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>

#include <xcb/xcb.h>
#include <xcb/xfixes.h>

#include <wayland-server.h>

#include "compositor.h"
#include "xserver-server-protocol.h"
#include "hash.h"

struct xserver {
	struct wl_resource resource;
};

struct wlsc_xserver {
	struct wl_display *wl_display;
	struct wl_event_loop *loop;
	struct wl_event_source *sigchld_source;
	int abstract_fd;
	struct wl_event_source *abstract_source;
	int unix_fd;
	struct wl_event_source *unix_source;
	int display;
	struct wlsc_process process;
	struct wl_resource *resource;
	struct wl_client *client;
	struct wlsc_compositor *compositor;
	struct wlsc_wm *wm;
};

struct wlsc_wm {
	xcb_connection_t *conn;
	const xcb_query_extension_reply_t *xfixes;
	struct wl_event_source *source;
	xcb_screen_t *screen;
	struct hash_table *window_hash;
	struct wlsc_xserver *server;

	xcb_window_t selection_window;
	int incr;
	int data_source_fd;
	struct wl_event_source *property_source;
	xcb_get_property_reply_t *property_reply;
	int property_start;

	struct {
		xcb_atom_t		 wm_protocols;
		xcb_atom_t		 wm_take_focus;
		xcb_atom_t		 wm_delete_window;
		xcb_atom_t		 net_wm_name;
		xcb_atom_t		 net_wm_icon;
		xcb_atom_t		 net_wm_state;
		xcb_atom_t		 net_wm_state_fullscreen;
		xcb_atom_t		 net_wm_user_time;
		xcb_atom_t		 net_wm_icon_name;
		xcb_atom_t		 net_wm_window_type;
		xcb_atom_t		 clipboard;
		xcb_atom_t		 targets;
		xcb_atom_t		 utf8_string;
		xcb_atom_t		 wl_selection;
		xcb_atom_t		 incr;
		xcb_atom_t		 timestamp;
		xcb_atom_t		 multiple;
		xcb_atom_t		 compound_text;
		xcb_atom_t		 text;
		xcb_atom_t		 string;
		xcb_atom_t		 text_plain_utf8;
		xcb_atom_t		 text_plain;
	} atom;
};

struct wlsc_wm_window {
	xcb_window_t id;
	struct wlsc_surface *surface;
	struct wl_listener surface_destroy_listener;
	char *class;
	char *name;
	struct wlsc_wm_window *transient_for;
	uint32_t protocols;
	xcb_atom_t type;
};

static struct wlsc_wm_window *
get_wm_window(struct wlsc_surface *surface);

static const char *
get_atom_name(xcb_connection_t *c, xcb_atom_t atom)
{
	xcb_get_atom_name_cookie_t cookie;
	xcb_get_atom_name_reply_t *reply;
	xcb_generic_error_t *e;
	static char buffer[64];

	if (atom == XCB_ATOM_NONE)
		return "None";

	cookie = xcb_get_atom_name (c, atom);
	reply = xcb_get_atom_name_reply (c, cookie, &e);
	snprintf(buffer, sizeof buffer, "%.*s",
		 xcb_get_atom_name_name_length (reply),
		 xcb_get_atom_name_name (reply));
	free(reply);

	return buffer;
}

static void
dump_property(struct wlsc_wm *wm, xcb_atom_t property,
	      xcb_get_property_reply_t *reply)
{
	int32_t *incr_value;
	const char *text_value, *name;
	xcb_atom_t *atom_value;
	int i, width, len;

	width = fprintf(stderr, "  %s: ", get_atom_name(wm->conn, property));
	if (reply == NULL) {
		fprintf(stderr, "(no reply)\n");
		return;
	}

	width += fprintf(stderr,
			 "type %s, format %d, length %d (value_len %d): ",
			 get_atom_name(wm->conn, reply->type),
			 reply->format,
			 xcb_get_property_value_length(reply),
			 reply->value_len);

	if (reply->type == wm->atom.incr) {
		incr_value = xcb_get_property_value(reply);
		fprintf(stderr, "%d\n", *incr_value);
	} else if (reply->type == wm->atom.utf8_string ||
	      reply->type == wm->atom.string) {
		text_value = xcb_get_property_value(reply);
		if (reply->value_len > 40)
			len = 40;
		else
			len = reply->value_len;
		fprintf(stderr, "\"%.*s\"\n", len, text_value);
	} else if (reply->type == XCB_ATOM_ATOM) {
		atom_value = xcb_get_property_value(reply);
		for (i = 0; i < reply->value_len; i++) {
			name = get_atom_name(wm->conn, atom_value[i]);
			if (width + strlen(name) + 2 > 78) {
				fprintf(stderr, "\n    ");
				width = 4;
			} else if (i > 0) {
				width += fprintf(stderr, ", ");
			}

			width += fprintf(stderr, "%s", name);
		}
		fprintf(stderr, "\n");
	} else {
		fprintf(stderr, "huh?\n");
	}
}

static void
dump_window_properties(struct wlsc_wm *wm, xcb_window_t window)
{
	xcb_list_properties_cookie_t list_cookie;
	xcb_list_properties_reply_t *list_reply;
	xcb_get_property_cookie_t property_cookie;
	xcb_get_property_reply_t *property_reply;
	xcb_atom_t *atoms;
	int i, length;

	list_cookie = xcb_list_properties(wm->conn, window);
	list_reply = xcb_list_properties_reply(wm->conn, list_cookie, NULL);
	if (!list_reply)
		/* Bad window, typically */
		return;

	length = xcb_list_properties_atoms_length(list_reply);
	atoms = xcb_list_properties_atoms(list_reply);

	for (i = 0; i < length; i++) {
		property_cookie =
			xcb_get_property(wm->conn,
					 0, /* delete */
					 window,
					 atoms[i],
					 XCB_ATOM_ANY,
					 0, 2048);

		property_reply = xcb_get_property_reply(wm->conn,
							property_cookie, NULL);
		dump_property(wm, atoms[i], property_reply);

		free(property_reply);
	}

	free(list_reply);
}

static void
data_offer_accept(struct wl_client *client, struct wl_resource *resource,
		  uint32_t time, const char *mime_type)
{
	struct wlsc_data_source *source = resource->data;

	wl_resource_post_event(&source->resource,
			       WL_DATA_SOURCE_TARGET, mime_type);
}

static void
data_offer_receive(struct wl_client *client, struct wl_resource *resource,
		   const char *mime_type, int32_t fd)
{
	struct wlsc_data_source *source = resource->data;
	struct wlsc_wm *wm = source->data;

	if (strcmp(mime_type, "text/plain;charset=utf-8") == 0) {
		/* Get data for the utf8_string target */
		xcb_convert_selection(wm->conn,
				      wm->selection_window,
				      wm->atom.clipboard,
				      wm->atom.utf8_string,
				      wm->atom.wl_selection,
				      XCB_TIME_CURRENT_TIME);

		xcb_flush(wm->conn);

		fcntl(fd, F_SETFL, O_WRONLY | O_NONBLOCK);
		wm->data_source_fd = fd;
	} else {
		close(fd);
	}
}

static void
data_offer_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource, wlsc_compositor_get_time());
}

static void
destroy_data_offer(struct wl_resource *resource)
{
	struct wlsc_data_source *source = resource->data;

	wlsc_data_source_unref(source);
	free(resource);
}

static const struct wl_data_offer_interface data_offer_interface = {
	data_offer_accept,
	data_offer_receive,
	data_offer_destroy,
};

static struct wl_resource *
data_source_create_offer(struct wlsc_data_source *source, 
			 struct wl_resource *target)
{
	struct wl_resource *resource;

	resource = wl_client_new_object(target->client,
					&wl_data_offer_interface,
					&data_offer_interface, source);
	resource->destroy = destroy_data_offer;

	return resource;
}

static void
data_source_cancel(struct wlsc_data_source *source)
{
}

static void
wlsc_wm_get_selection_targets(struct wlsc_wm *wm)
{
	struct wlsc_data_source *source;
	struct wlsc_input_device *device;
	xcb_get_property_cookie_t cookie;
	xcb_get_property_reply_t *reply;
	xcb_atom_t *value;
	char **p;
	int i;

	cookie = xcb_get_property(wm->conn,
				  1, /* delete */
				  wm->selection_window,
				  wm->atom.wl_selection,
				  XCB_GET_PROPERTY_TYPE_ANY,
				  0, /* offset */
				  4096 /* length */);

	reply = xcb_get_property_reply(wm->conn, cookie, NULL);

	dump_property(wm, wm->atom.wl_selection, reply);

	if (reply->type != XCB_ATOM_ATOM) {
		free(reply);
		return;
	}

	source = malloc(sizeof *source);
	if (source == NULL)
		return;

	wl_list_init(&source->resource.destroy_listener_list);
	source->create_offer = data_source_create_offer;
	source->cancel = data_source_cancel;
	source->data = wm;
	source->refcount = 1;

	wl_array_init(&source->mime_types);
	value = xcb_get_property_value(reply);
	for (i = 0; i < reply->value_len; i++) {
		if (value[i] == wm->atom.utf8_string) {
			p = wl_array_add(&source->mime_types, sizeof *p);
			if (p)
				*p = strdup("text/plain;charset=utf-8");
		}
	}

	device = (struct wlsc_input_device *)
		wm->server->compositor->input_device;
	wlsc_input_device_set_selection(device, source,
					wlsc_compositor_get_time());

	wlsc_data_source_unref(source);
	free(reply);
}

static int
wlsc_wm_write_property(int fd, uint32_t mask, void *data)
{
	struct wlsc_wm *wm = data;
	unsigned char *property;
	int len, remainder;

	property = xcb_get_property_value(wm->property_reply);
	remainder = xcb_get_property_value_length(wm->property_reply) -
		wm->property_start;

	len = write(fd, property + wm->property_start, remainder);
	if (len == -1) {
		free(wm->property_reply);
		wl_event_source_remove(wm->property_source);
		close(fd);
		fprintf(stderr, "write error to target fd: %m\n");
		return 1;
	}

	fprintf(stderr, "wrote %d (chunk size %d) of %d bytes\n",
		wm->property_start + len,
		len, xcb_get_property_value_length(wm->property_reply));

	wm->property_start += len;
	if (len == remainder) {
		free(wm->property_reply);
		wl_event_source_remove(wm->property_source);

		if (wm->incr) {
			xcb_delete_property(wm->conn,
					    wm->selection_window,
					    wm->atom.wl_selection);
		} else {
			fprintf(stderr, "transfer complete\n");
			close(fd);
		}
	}

	return 1;
}

static void
wlsc_wm_get_selection_data(struct wlsc_wm *wm)
{
	xcb_get_property_cookie_t cookie;
	xcb_get_property_reply_t *reply;

	cookie = xcb_get_property(wm->conn,
				  1, /* delete */
				  wm->selection_window,
				  wm->atom.wl_selection,
				  XCB_GET_PROPERTY_TYPE_ANY,
				  0, /* offset */
				  0x1fffffff /* length */);

	reply = xcb_get_property_reply(wm->conn, cookie, NULL);

	if (reply->type == wm->atom.incr) {
		dump_property(wm, wm->atom.wl_selection, reply);
		wm->incr = 1;
		free(reply);
	} else {
		dump_property(wm, wm->atom.wl_selection, reply);
		wm->incr = 0;
		wm->property_start = 0;
		wm->property_source =
			wl_event_loop_add_fd(wm->server->loop,
					     wm->data_source_fd,
					     WL_EVENT_WRITEABLE,
					     wlsc_wm_write_property,
					     wm);
		wm->property_reply = reply;
	}
}

static void
wlsc_wm_get_incr_chunk(struct wlsc_wm *wm)
{
	xcb_get_property_cookie_t cookie;
	xcb_get_property_reply_t *reply;

	cookie = xcb_get_property(wm->conn,
				  0, /* delete */
				  wm->selection_window,
				  wm->atom.wl_selection,
				  XCB_GET_PROPERTY_TYPE_ANY,
				  0, /* offset */
				  0x1fffffff /* length */);

	reply = xcb_get_property_reply(wm->conn, cookie, NULL);

	dump_property(wm, wm->atom.wl_selection, reply);

	if (xcb_get_property_value_length(reply) > 0) {
		wm->property_start = 0;
		wm->property_source =
			wl_event_loop_add_fd(wm->server->loop,
					     wm->data_source_fd,
					     WL_EVENT_WRITEABLE,
					     wlsc_wm_write_property,
					     wm);
		wm->property_reply = reply;
	} else {
		fprintf(stderr, "transfer complete\n");
		close(wm->data_source_fd);
		free(reply);
	}
}

static void
wlsc_wm_handle_configure_request(struct wlsc_wm *wm, xcb_generic_event_t *event)
{
	xcb_configure_request_event_t *configure_request = 
		(xcb_configure_request_event_t *) event;
	uint32_t values[16];
	int i = 0;

	fprintf(stderr, "XCB_CONFIGURE_REQUEST (window %d) %d,%d @ %dx%d\n",
		configure_request->window,
		configure_request->x, configure_request->y,
		configure_request->width, configure_request->height);

	if (configure_request->value_mask & XCB_CONFIG_WINDOW_X)
		values[i++] = configure_request->x;
	if (configure_request->value_mask & XCB_CONFIG_WINDOW_Y)
		values[i++] = configure_request->y;
	if (configure_request->value_mask & XCB_CONFIG_WINDOW_WIDTH)
		values[i++] = configure_request->width;
	if (configure_request->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
		values[i++] = configure_request->height;
	if (configure_request->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH)
		values[i++] = configure_request->border_width;
	if (configure_request->value_mask & XCB_CONFIG_WINDOW_SIBLING)
		values[i++] = configure_request->sibling;
	if (configure_request->value_mask & XCB_CONFIG_WINDOW_STACK_MODE)
		values[i++] = configure_request->stack_mode;

	xcb_configure_window(wm->conn,
			     configure_request->window,
			     configure_request->value_mask, values);
}

static void
wlsc_wm_handle_configure_notify(struct wlsc_wm *wm, xcb_generic_event_t *event)
{
	xcb_configure_notify_event_t *configure_notify = 
		(xcb_configure_notify_event_t *) event;

	fprintf(stderr, "XCB_CONFIGURE_NOTIFY (window %d) %d,%d @ %dx%d\n",
		configure_notify->window,
		configure_notify->x, configure_notify->y,
		configure_notify->width, configure_notify->height);
}

static void
wlsc_wm_activate(struct wlsc_wm *wm,
		 struct wlsc_wm_window *window, xcb_timestamp_t time)
{
	xcb_client_message_event_t client_message;

	client_message.response_type = XCB_CLIENT_MESSAGE;
	client_message.format = 32;
	client_message.window = window->id;
	client_message.type = wm->atom.wm_protocols;
	client_message.data.data32[0] = wm->atom.wm_take_focus;
	client_message.data.data32[1] = XCB_TIME_CURRENT_TIME;

	xcb_send_event(wm->conn, 0, window->id, 
		       XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
		       (char *) &client_message);

	xcb_set_input_focus (wm->conn,
			     XCB_INPUT_FOCUS_POINTER_ROOT, window->id, time);
}

WL_EXPORT void
wlsc_xserver_surface_activate(struct wlsc_surface *surface)
{
	struct wlsc_wm_window *window = get_wm_window(surface);
	struct wlsc_xserver *wxs = surface->compositor->wxs;

	if (window)
		wlsc_wm_activate(wxs->wm, window, XCB_TIME_CURRENT_TIME);
	else if (wxs && wxs->wm)
		xcb_set_input_focus (wxs->wm->conn,
				     XCB_INPUT_FOCUS_POINTER_ROOT,
				     XCB_NONE,
				     XCB_TIME_CURRENT_TIME);
}

static void
wlsc_wm_handle_map_request(struct wlsc_wm *wm, xcb_generic_event_t *event)
{
	xcb_map_request_event_t *map_request =
		(xcb_map_request_event_t *) event;
	uint32_t values[1];

	fprintf(stderr, "XCB_MAP_REQUEST (window %d)\n", map_request->window);

	values[0] = XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes(wm->conn, map_request->window,
				     XCB_CW_EVENT_MASK, values);

	xcb_map_window(wm->conn, map_request->window);
}

/* We reuse some predefined, but otherwise useles atoms */
#define TYPE_WM_PROTOCOLS XCB_ATOM_CUT_BUFFER0

static void
wlsc_wm_handle_map_notify(struct wlsc_wm *wm, xcb_generic_event_t *event)
{
#define F(field) offsetof(struct wlsc_wm_window, field)

	const struct {
		xcb_atom_t atom;
		xcb_atom_t type;
		int offset;
	} props[] = {
		{ XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, F(class) },
		{ XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, F(transient_for) },
		{ wm->atom.wm_protocols, TYPE_WM_PROTOCOLS, F(protocols) },
		{ wm->atom.net_wm_window_type, XCB_ATOM_ATOM, F(type) },
		{ wm->atom.net_wm_name, XCB_ATOM_STRING, F(name) },
	};
#undef F

	xcb_map_notify_event_t *map_notify = (xcb_map_notify_event_t *) event;
	xcb_get_property_cookie_t cookie[ARRAY_LENGTH(props)];
	xcb_get_property_reply_t *reply;
	struct wlsc_wm_window *window;
	void *p;
	uint32_t *xid;
	xcb_atom_t *atom;
	int i;

	fprintf(stderr, "XCB_MAP_NOTIFY (window %d)\n", map_notify->window);

	dump_window_properties(wm, map_notify->window);

	window = hash_table_lookup(wm->window_hash, map_notify->window);

	for (i = 0; i < ARRAY_LENGTH(props); i++)
		cookie[i] = xcb_get_property(wm->conn,
					     0, /* delete */
					     window->id,
					     props[i].atom,
					     XCB_ATOM_ANY, 0, 2048);

	for (i = 0; i < ARRAY_LENGTH(props); i++)  {
		reply = xcb_get_property_reply(wm->conn, cookie[i], NULL);
		if (!reply)
			/* Bad window, typically */
			continue;

		p = ((char *) window + props[i].offset);

		switch (props[i].type) {
		case XCB_ATOM_STRING:
			/* FIXME: We're using this for both string and
			   utf8_string */
			*(char **) p =
				strndup(xcb_get_property_value(reply),
					xcb_get_property_value_length(reply));
			break;
		case XCB_ATOM_WINDOW:
			xid = xcb_get_property_value(reply);
			*(struct wlsc_wm_window **) p =
				hash_table_lookup(wm->window_hash, *xid);
			break;
		case XCB_ATOM_ATOM:
			atom = xcb_get_property_value(reply);
			*(xcb_atom_t *) p = *atom;
			break;
		case TYPE_WM_PROTOCOLS:
			break;
		default:
			break;
		}
		free(reply);
	}

	fprintf(stderr, "window %d: name %s, class %s, transient_for %d\n",
		window->id, window->name, window->class,
		window->transient_for ? window->transient_for->id : 0);
	wlsc_wm_activate(wm, window, XCB_TIME_CURRENT_TIME);
}

static void
wlsc_wm_handle_property_notify(struct wlsc_wm *wm, xcb_generic_event_t *event)
{
	xcb_property_notify_event_t *property_notify =
		(xcb_property_notify_event_t *) event;

	if (property_notify->window == wm->selection_window) {
		if (property_notify->state == XCB_PROPERTY_NEW_VALUE &&
		    property_notify->atom == wm->atom.wl_selection &&
		    wm->incr)
			wlsc_wm_get_incr_chunk(wm);
	} else if (property_notify->atom == XCB_ATOM_WM_CLASS) {
		fprintf(stderr, "wm_class changed\n");
	} else if (property_notify->atom == XCB_ATOM_WM_TRANSIENT_FOR) {
		fprintf(stderr, "wm_transient_for changed\n");
	} else if (property_notify->atom == wm->atom.wm_protocols) {
		fprintf(stderr, "wm_protocols changed\n");
	} else if (property_notify->atom == wm->atom.net_wm_name) {
		fprintf(stderr, "_net_wm_name changed\n");
	} else if (property_notify->atom == wm->atom.net_wm_user_time) {
		fprintf(stderr, "_net_wm_user_time changed\n");
	} else if (property_notify->atom == wm->atom.net_wm_icon_name) {
		fprintf(stderr, "_net_wm_icon_name changed\n");
	} else if (property_notify->atom == XCB_ATOM_WM_NAME) {
		fprintf(stderr, "wm_name changed\n");
	} else if (property_notify->atom == XCB_ATOM_WM_ICON_NAME) {
		fprintf(stderr, "wm_icon_name changed\n");
	} else {
		fprintf(stderr, "XCB_PROPERTY_NOTIFY: "
			"unhandled property change: %s\n",
			get_atom_name(wm->conn, property_notify->atom));
	}
}

static void
wlsc_wm_handle_create_notify(struct wlsc_wm *wm, xcb_generic_event_t *event)
{
	xcb_create_notify_event_t *create_notify =
		(xcb_create_notify_event_t *) event;
	struct wlsc_wm_window *window;

	fprintf(stderr, "XCB_CREATE_NOTIFY (window %d)\n",
		create_notify->window);

	window = malloc(sizeof *window);
	if (window == NULL) {
		fprintf(stderr, "failed to allocate window\n");
		return;
	}

	memset(window, 0, sizeof *window);
	window->id = create_notify->window;
	hash_table_insert(wm->window_hash, window->id, window);
}

static void
wlsc_wm_handle_destroy_notify(struct wlsc_wm *wm, xcb_generic_event_t *event)
{
	xcb_destroy_notify_event_t *destroy_notify =
		(xcb_destroy_notify_event_t *) event;
	struct wlsc_wm_window *window;

	fprintf(stderr, "XCB_DESTROY_NOTIFY, win %d\n",
		destroy_notify->window);

	window = hash_table_lookup(wm->window_hash, destroy_notify->window);
	if (window == NULL) {
		fprintf(stderr, "destroy notify for unknow window %d\n",
			destroy_notify->window);
		return;
	}

	fprintf(stderr, "destroy window %p\n", window);
	hash_table_remove(wm->window_hash, window->id);
	if (window->surface)
		wl_list_remove(&window->surface_destroy_listener.link);
	free(window);
}

static void
wlsc_wm_handle_selection_notify(struct wlsc_wm *wm,
				xcb_generic_event_t *event)
{
	xcb_selection_notify_event_t *selection_notify =
		(xcb_selection_notify_event_t *) event;

	if (selection_notify->property == XCB_ATOM_NONE) {
		/* convert selection failed */
	} else if (selection_notify->target == wm->atom.targets) {
		wlsc_wm_get_selection_targets(wm);
	} else {
		wlsc_wm_get_selection_data(wm);
	}
}

static void
wlsc_wm_handle_xfixes_selection_notify(struct wlsc_wm *wm,
				       xcb_generic_event_t *event)
{
	xcb_xfixes_selection_notify_event_t *xfixes_selection_notify =
		(xcb_xfixes_selection_notify_event_t *) event;

	printf("xfixes selection notify event: owner %d\n",
	       xfixes_selection_notify->owner);

	xcb_convert_selection(wm->conn, wm->selection_window,
			      wm->atom.clipboard,
			      wm->atom.targets,
			      wm->atom.wl_selection,
			      XCB_TIME_CURRENT_TIME);

	xcb_flush(wm->conn);
}

static int
wlsc_wm_handle_event(int fd, uint32_t mask, void *data)
{
	struct wlsc_wm *wm = data;
	xcb_generic_event_t *event;
	int count = 0;

	while (event = xcb_poll_for_event(wm->conn), event != NULL) {
		switch (event->response_type & ~0x80) {
		case XCB_CREATE_NOTIFY:
			wlsc_wm_handle_create_notify(wm, event);
			break;
		case XCB_MAP_REQUEST:
			wlsc_wm_handle_map_request(wm, event);
			break;
		case XCB_MAP_NOTIFY:
			wlsc_wm_handle_map_notify(wm, event);
			break;
		case XCB_UNMAP_NOTIFY:
			fprintf(stderr, "XCB_UNMAP_NOTIFY\n");
			break;
		case XCB_CONFIGURE_REQUEST:
			wlsc_wm_handle_configure_request(wm, event);
			break;
		case XCB_CONFIGURE_NOTIFY:
			wlsc_wm_handle_configure_notify(wm, event);
			break;
		case XCB_DESTROY_NOTIFY:
			wlsc_wm_handle_destroy_notify(wm, event);
			break;
		case XCB_MAPPING_NOTIFY:
			fprintf(stderr, "XCB_MAPPING_NOTIFY\n");
			break;
		case XCB_PROPERTY_NOTIFY:
			wlsc_wm_handle_property_notify(wm, event);
			break;
		case XCB_SELECTION_NOTIFY:
			wlsc_wm_handle_selection_notify(wm, event);
			break;
		}

		switch (event->response_type - wm->xfixes->first_event) {
		case XCB_XFIXES_SELECTION_NOTIFY:
			wlsc_wm_handle_xfixes_selection_notify(wm, event);
			break;
		}


		free(event);
		count++;
	}

	xcb_flush(wm->conn);

	return count;
}

static void
wxs_wm_get_resources(struct wlsc_wm *wm)
{

#define F(field) offsetof(struct wlsc_wm, field)

	static const struct { const char *name; int offset; } atoms[] = {
		{ "WM_PROTOCOLS",	F(atom.wm_protocols) },
		{ "WM_TAKE_FOCUS",	F(atom.wm_take_focus) },
		{ "WM_DELETE_WINDOW",	F(atom.wm_delete_window) },
		{ "_NET_WM_NAME",	F(atom.net_wm_name) },
		{ "_NET_WM_ICON",	F(atom.net_wm_icon) },
		{ "_NET_WM_STATE",	F(atom.net_wm_state) },
		{ "_NET_WM_STATE_FULLSCREEN", F(atom.net_wm_state_fullscreen) },
		{ "_NET_WM_USER_TIME", F(atom.net_wm_user_time) },
		{ "_NET_WM_ICON_NAME", F(atom.net_wm_icon_name) },
		{ "_NET_WM_WINDOW_TYPE", F(atom.net_wm_window_type) },
		{ "CLIPBOARD",		F(atom.clipboard) },
		{ "TARGETS",		F(atom.targets) },
		{ "UTF8_STRING",	F(atom.utf8_string) },
		{ "_WL_SELECTION",	F(atom.wl_selection) },
		{ "INCR",		F(atom.incr) },
		{ "TIMESTAMP",		F(atom.timestamp) },
		{ "MULTIPLE",		F(atom.multiple) },
		{ "UTF8_STRING"	,	F(atom.utf8_string) },
		{ "COMPOUND_TEXT",	F(atom.compound_text) },
		{ "TEXT",		F(atom.text) },
		{ "STRING",		F(atom.string) },
		{ "text/plain;charset=utf-8",	F(atom.text_plain_utf8) },
		{ "text/plain",		F(atom.text_plain) },
	};

	xcb_xfixes_query_version_cookie_t xfixes_cookie;
	xcb_xfixes_query_version_reply_t *xfixes_reply;
	xcb_intern_atom_cookie_t cookies[ARRAY_LENGTH(atoms)];
	xcb_intern_atom_reply_t *reply;
	int i;

	xcb_prefetch_extension_data (wm->conn, &xcb_xfixes_id);

	for (i = 0; i < ARRAY_LENGTH(atoms); i++)
		cookies[i] = xcb_intern_atom (wm->conn, 0,
					      strlen(atoms[i].name),
					      atoms[i].name);

	for (i = 0; i < ARRAY_LENGTH(atoms); i++) {
		reply = xcb_intern_atom_reply (wm->conn, cookies[i], NULL);
		*(xcb_atom_t *) ((char *) wm + atoms[i].offset) = reply->atom;
		free(reply);
	}

	wm->xfixes = xcb_get_extension_data(wm->conn, &xcb_xfixes_id);
	if (!wm->xfixes || !wm->xfixes->present)
		fprintf(stderr, "xfixes not available\n");

	xfixes_cookie = xcb_xfixes_query_version(wm->conn,
						 XCB_XFIXES_MAJOR_VERSION,
						 XCB_XFIXES_MINOR_VERSION);
	xfixes_reply = xcb_xfixes_query_version_reply(wm->conn,
						      xfixes_cookie, NULL);

	printf("xfixes version: %d.%d\n",
	       xfixes_reply->major_version, xfixes_reply->minor_version);

	free(xfixes_reply);
}

static struct wlsc_wm *
wlsc_wm_create(struct wlsc_xserver *wxs)
{
	struct wlsc_wm *wm;
	struct wl_event_loop *loop;
	xcb_screen_iterator_t s;
	uint32_t values[1], mask;
	int sv[2];

	wm = malloc(sizeof *wm);
	if (wm == NULL)
		return NULL;

	wm->server = wxs;
	wm->window_hash = hash_table_create();
	if (wm->window_hash == NULL) {
		free(wm);
		return NULL;
	}

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
		fprintf(stderr, "socketpair failed\n");
		hash_table_destroy(wm->window_hash);
		free(wm);
		return NULL;
	}

	wl_resource_post_event(wxs->resource, XSERVER_CLIENT, sv[1]);
	wl_client_flush(wxs->resource->client);
	close(sv[1]);
	
	/* xcb_connect_to_fd takes ownership of the fd. */
	wm->conn = xcb_connect_to_fd(sv[0], NULL);
	if (xcb_connection_has_error(wm->conn)) {
		fprintf(stderr, "xcb_connect_to_fd failed\n");
		close(sv[0]);
		hash_table_destroy(wm->window_hash);
		free(wm);
		return NULL;
	}

	s = xcb_setup_roots_iterator(xcb_get_setup(wm->conn));
	wm->screen = s.data;

	loop = wl_display_get_event_loop(wxs->wl_display);
	wm->source =
		wl_event_loop_add_fd(loop, sv[0],
				     WL_EVENT_READABLE,
				     wlsc_wm_handle_event, wm);
	wl_event_source_check(wm->source);

	wxs_wm_get_resources(wm);

	values[0] =
		XCB_EVENT_MASK_STRUCTURE_NOTIFY |
		XCB_EVENT_MASK_RESIZE_REDIRECT |
		XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
		XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
		XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes(wm->conn, wm->screen->root,
				     XCB_CW_EVENT_MASK, values);

	wm->selection_window = xcb_generate_id(wm->conn);
	xcb_create_window(wm->conn,
			  XCB_COPY_FROM_PARENT,
			  wm->selection_window,
			  wm->screen->root,
			  0, 0,
			  10, 10,
			  0,
			  XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  wm->screen->root_visual,
			  XCB_CW_EVENT_MASK, values);

	mask =
		XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
		XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
		XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE;

	xcb_xfixes_select_selection_input(wm->conn, wm->selection_window,
					  wm->atom.clipboard, mask);

	xcb_flush(wm->conn);
	fprintf(stderr, "created wm\n");

	return wm;
}

static void
wlsc_wm_destroy(struct wlsc_wm *wm)
{
	/* FIXME: Free windows in hash. */
	hash_table_destroy(wm->window_hash);
	xcb_disconnect(wm->conn);
	wl_event_source_remove(wm->source);
	free(wm);
}

static int
wlsc_xserver_handle_event(int listen_fd, uint32_t mask, void *data)
{
	struct wlsc_xserver *mxs = data;
	char display[8], s[8], logfile[32];
	int sv[2], flags;

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
		fprintf(stderr, "socketpair failed\n");
		return 1;
	}

	mxs->process.pid = fork();
	switch (mxs->process.pid) {
	case 0:
		/* SOCK_CLOEXEC closes both ends, so we need to unset
		 * the flag on the client fd. */
		flags = fcntl(sv[1], F_GETFD);
		if (flags != -1)
			fcntl(sv[1], F_SETFD, flags & ~FD_CLOEXEC);

		snprintf(s, sizeof s, "%d", sv[1]);
		setenv("WAYLAND_SOCKET", s, 1);

		snprintf(display, sizeof display, ":%d", mxs->display);
		snprintf(logfile, sizeof logfile,
			 "/tmp/x-log-%d", mxs->display);

		if (execl(XSERVER_PATH,
			  XSERVER_PATH,
			  display,
			  "-wayland",
			  "-rootless",
			  "-retro",
			  "-logfile", logfile,
			  "-nolisten", "all",
			  "-terminate",
			  NULL) < 0)
			fprintf(stderr, "exec failed: %m\n");
		exit(-1);

	default:
		fprintf(stderr, "forked X server, pid %d\n", mxs->process.pid);

		close(sv[1]);
		mxs->client = wl_client_create(mxs->wl_display, sv[0]);

		wlsc_watch_process(&mxs->process);

		wl_event_source_remove(mxs->abstract_source);
		wl_event_source_remove(mxs->unix_source);
		break;

	case -1:
		fprintf(stderr, "failed to fork\n");
		break;
	}

	return 1;
}

static void
wlsc_xserver_shutdown(struct wlsc_xserver *wxs)
{
	char path[256];

	snprintf(path, sizeof path, "/tmp/.X%d-lock", wxs->display);
	unlink(path);
	snprintf(path, sizeof path, "/tmp/.X11-unix/X%d", wxs->display);
	unlink(path);
	if (wxs->process.pid == 0) {
		wl_event_source_remove(wxs->abstract_source);
		wl_event_source_remove(wxs->unix_source);
	}
	close(wxs->abstract_fd);
	close(wxs->unix_fd);
	if (wxs->wm)
		wlsc_wm_destroy(wxs->wm);
	wxs->loop = NULL;
}

static void
wlsc_xserver_cleanup(struct wlsc_process *process, int status)
{
	struct wlsc_xserver *mxs =
		container_of(process, struct wlsc_xserver, process);

	mxs->process.pid = 0;
	mxs->client = NULL;
	mxs->resource = NULL;

	mxs->abstract_source =
		wl_event_loop_add_fd(mxs->loop, mxs->abstract_fd,
				     WL_EVENT_READABLE,
				     wlsc_xserver_handle_event, mxs);

	mxs->unix_source =
		wl_event_loop_add_fd(mxs->loop, mxs->unix_fd,
				     WL_EVENT_READABLE,
				     wlsc_xserver_handle_event, mxs);

	if (mxs->wm) {
		fprintf(stderr, "xserver exited, code %d\n", status);
		wlsc_wm_destroy(mxs->wm);
		mxs->wm = NULL;
	} else {
		/* If the X server crashes before it binds to the
		 * xserver interface, shut down and don't try
		 * again. */
		fprintf(stderr, "xserver crashing too fast: %d\n", status);
		wlsc_xserver_shutdown(mxs);
	}
}

static void
surface_destroy(struct wl_listener *listener,
		struct wl_resource *resource, uint32_t time)
{
	struct wlsc_wm_window *window =
		container_of(listener,
			     struct wlsc_wm_window, surface_destroy_listener);

	fprintf(stderr, "surface for xid %d destroyed\n", window->id);
}

static struct wlsc_wm_window *
get_wm_window(struct wlsc_surface *surface)
{
	struct wl_resource *resource = &surface->surface.resource;
	struct wl_listener *listener;

	wl_list_for_each(listener, &resource->destroy_listener_list, link) {
		if (listener->func == surface_destroy)
			return container_of(listener,
					    struct wlsc_wm_window,
					    surface_destroy_listener);
	}

	return NULL;
}

static void
xserver_set_window_id(struct wl_client *client, struct wl_resource *resource,
		      struct wl_resource *surface_resource, uint32_t id)
{
	struct wlsc_xserver *wxs = resource->data;
	struct wlsc_wm *wm = wxs->wm;
	struct wl_surface *surface = surface_resource->data;
	struct wlsc_wm_window *window;

	if (client != wxs->client)
		return;

	window = hash_table_lookup(wm->window_hash, id);
	if (window == NULL) {
		fprintf(stderr, "set_window_id for unknown window %d\n", id);
		return;
	}

	fprintf(stderr, "set_window_id %d for surface %p\n", id, surface);

	window->surface = (struct wlsc_surface *) surface;
	window->surface_destroy_listener.func = surface_destroy;
	wl_list_insert(surface->resource.destroy_listener_list.prev,
		       &window->surface_destroy_listener.link);
}

static const struct xserver_interface xserver_implementation = {
	xserver_set_window_id
};

static void
bind_xserver(struct wl_client *client,
	     void *data, uint32_t version, uint32_t id)
{
	struct wlsc_xserver *wxs = data;

	/* If it's a different client than the xserver we launched,
	 * don't start the wm. */
	if (client != wxs->client)
		return;

	wxs->resource = 
		wl_client_add_object(client, &xserver_interface,
				     &xserver_implementation, id, wxs);

	wxs->wm = wlsc_wm_create(wxs);
	if (wxs->wm == NULL) {
		fprintf(stderr, "failed to create wm\n");
	}

	wl_resource_post_event(wxs->resource,
			       XSERVER_LISTEN_SOCKET, wxs->abstract_fd);

	wl_resource_post_event(wxs->resource,
			       XSERVER_LISTEN_SOCKET, wxs->unix_fd);
}

static int
bind_to_abstract_socket(int display)
{
	struct sockaddr_un addr;
	socklen_t size, name_size;
	int fd;

	fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	addr.sun_family = AF_LOCAL;
	name_size = snprintf(addr.sun_path, sizeof addr.sun_path,
			     "%c/tmp/.X11-unix/X%d", 0, display);
	size = offsetof(struct sockaddr_un, sun_path) + name_size;
	if (bind(fd, (struct sockaddr *) &addr, size) < 0) {
		fprintf(stderr, "failed to bind to @%s: %s\n",
			addr.sun_path + 1, strerror(errno));
		close(fd);
		return -1;
	}

	if (listen(fd, 1) < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

static int
bind_to_unix_socket(int display)
{
	struct sockaddr_un addr;
	socklen_t size, name_size;
	int fd;

	fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	addr.sun_family = AF_LOCAL;
	name_size = snprintf(addr.sun_path, sizeof addr.sun_path,
			     "/tmp/.X11-unix/X%d", display) + 1;
	size = offsetof(struct sockaddr_un, sun_path) + name_size;
	unlink(addr.sun_path);
	if (bind(fd, (struct sockaddr *) &addr, size) < 0) {
		fprintf(stderr, "failed to bind to %s (%s)\n",
			addr.sun_path, strerror(errno));
		close(fd);
		return -1;
	}

	if (listen(fd, 1) < 0) {
		unlink(addr.sun_path);
		close(fd);
		return -1;
	}

	return fd;
}

static int
create_lockfile(int display, char *lockfile, size_t lsize)
{
	char pid[16], *end;
	int fd, size;
	pid_t other;

	snprintf(lockfile, lsize, "/tmp/.X%d-lock", display);
	fd = open(lockfile, O_WRONLY | O_CLOEXEC | O_CREAT | O_EXCL, 0444);
	if (fd < 0 && errno == EEXIST) {
		fd = open(lockfile, O_CLOEXEC, O_RDONLY);
		if (fd < 0 || read(fd, pid, 11) != 11) {
			fprintf(stderr, "can't read lock file %s: %s\n",
				lockfile, strerror(errno));
			errno = EEXIST;
			return -1;
		}

		other = strtol(pid, &end, 0);
		if (end != pid + 10) {
			fprintf(stderr, "can't parse lock file %s\n",
				lockfile);
			errno = EEXIST;
			return -1;
		}

		if (kill(other, 0) < 0 && errno == ESRCH) {
			/* stale lock file; unlink and try again */
			fprintf(stderr,
				"unlinking stale lock file %s\n", lockfile);
			unlink(lockfile);
			errno = EAGAIN;
			return -1;
		}

		errno = EEXIST;
		return -1;
	} else if (fd < 0) {
		fprintf(stderr, "failed to create lock file %s: %s\n",
			lockfile, strerror(errno));
		return -1;
	}

	/* Subtle detail: we use the pid of the wayland
	 * compositor, not the xserver in the lock file. */
	size = snprintf(pid, sizeof pid, "%10d\n", getpid());
	if (write(fd, pid, size) != size) {
		unlink(lockfile);
		close(fd);
		return -1;
	}

	close(fd);

	return 0;
}

int
wlsc_xserver_init(struct wlsc_compositor *compositor)
{
	struct wl_display *display = compositor->wl_display;
	struct wlsc_xserver *mxs;
	char lockfile[256];

	mxs = malloc(sizeof *mxs);
	memset(mxs, 0, sizeof *mxs);

	mxs->process.cleanup = wlsc_xserver_cleanup;
	mxs->wl_display = display;
	mxs->compositor = compositor;

	mxs->display = 0;

 retry:
	if (create_lockfile(mxs->display, lockfile, sizeof lockfile) < 0) {
		if (errno == EAGAIN) {
			goto retry;
		} else if (errno == EEXIST) {
			mxs->display++;
			goto retry;
		} else {
			free(mxs);
			return -1;
		}
	}				

	mxs->abstract_fd = bind_to_abstract_socket(mxs->display);
	if (mxs->abstract_fd < 0 && errno == EADDRINUSE) {
		mxs->display++;
		unlink(lockfile);
		goto retry;
	}

	mxs->unix_fd = bind_to_unix_socket(mxs->display);
	if (mxs->unix_fd < 0) {
		unlink(lockfile);
		close(mxs->abstract_fd);
		free(mxs);
		return -1;
	}

	fprintf(stderr, "xserver listening on display :%d\n", mxs->display);

	mxs->loop = wl_display_get_event_loop(display);
	mxs->abstract_source =
		wl_event_loop_add_fd(mxs->loop, mxs->abstract_fd,
				     WL_EVENT_READABLE,
				     wlsc_xserver_handle_event, mxs);
	mxs->unix_source =
		wl_event_loop_add_fd(mxs->loop, mxs->unix_fd,
				     WL_EVENT_READABLE,
				     wlsc_xserver_handle_event, mxs);

	wl_display_add_global(display, &xserver_interface, mxs, bind_xserver);

	compositor->wxs = mxs;

	return 0;
}

void
wlsc_xserver_destroy(struct wlsc_compositor *compositor)
{
	struct wlsc_xserver *wxs = compositor->wxs;

	if (!wxs)
		return;

	if (wxs->loop)
		wlsc_xserver_shutdown(wxs);

	free(wxs);
}
