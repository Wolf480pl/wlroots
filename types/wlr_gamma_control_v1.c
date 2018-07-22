#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <wayland-server.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include "util/signal.h"
#include "wlr-gamma-control-unstable-v1-protocol.h"

#define GAMMA_CONTROL_MANAGER_V1_VERSION 1

static void gamma_control_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void gamma_control_destroy(struct wlr_gamma_control_v1 *gamma_control) {
	if (gamma_control == NULL) {
		return;
	}
	wlr_output_set_gamma(gamma_control->output, 0, NULL, NULL, NULL);
	wl_resource_set_user_data(gamma_control->resource, NULL);
	wl_list_remove(&gamma_control->output_destroy_listener.link);
	wl_list_remove(&gamma_control->link);
	free(gamma_control);
}

static void gamma_control_send_failed(
		struct wlr_gamma_control_v1 *gamma_control) {
	zwlr_gamma_control_v1_send_failed(gamma_control->resource);
	gamma_control_destroy(gamma_control);
}

static const struct zwlr_gamma_control_v1_interface gamma_control_impl;

static struct wlr_gamma_control_v1 *gamma_control_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &zwlr_gamma_control_v1_interface,
		&gamma_control_impl));
	return wl_resource_get_user_data(resource);
}

static void gamma_control_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_gamma_control_v1 *gamma_control =
		gamma_control_from_resource(resource);
	gamma_control_destroy(gamma_control);
}

static void gamma_control_handle_output_destroy(struct wl_listener *listener,
		void *data) {
	struct wlr_gamma_control_v1 *gamma_control =
		wl_container_of(listener, gamma_control, output_destroy_listener);
	gamma_control_destroy(gamma_control);
}

static void gamma_control_handle_set_gamma(struct wl_client *client,
		struct wl_resource *gamma_control_resource, int fd) {
	struct wlr_gamma_control_v1 *gamma_control =
		gamma_control_from_resource(gamma_control_resource);
	if (gamma_control == NULL) {
		goto error_fd;
	}

	uint32_t ramp_size = wlr_output_get_gamma_size(gamma_control->output);
	size_t table_size = ramp_size * 3 * sizeof(uint16_t);

	off_t fd_size = lseek(fd, 0, SEEK_END);
	// Skip checks if kernel does no support seek on buffer
	if (fd_size != -1 && (size_t)fd_size != table_size) {
		wl_resource_post_error(gamma_control_resource,
			ZWLR_GAMMA_CONTROL_V1_ERROR_INVALID_GAMMA,
			"The gamma ramps don't have the correct size");
		goto error_fd;
	}
	lseek(fd, 0, SEEK_SET);

	int fd_flags = fcntl(fd, F_GETFL, 0);
	if (fd_flags == -1) {
		gamma_control_send_failed(gamma_control);
		goto error_fd;
	}
	if (fcntl(fd, F_SETFL, fd_flags | O_NONBLOCK) == -1) {
		gamma_control_send_failed(gamma_control);
		goto error_fd;
	}

	// Use the heap since gamma tables can be large
	uint16_t *table = malloc(table_size);
	if (table == NULL) {
		wl_resource_post_no_memory(gamma_control_resource);
		goto error_fd;
	}

	ssize_t n_read = read(fd, table, table_size);
	if (n_read == -1 || (size_t)n_read != table_size) {
		gamma_control_send_failed(gamma_control);
		goto error_table;
	}

	uint16_t *r = table;
	uint16_t *g = table + ramp_size;
	uint16_t *b = table + 2 * ramp_size;

	bool ok = wlr_output_set_gamma(gamma_control->output, ramp_size, r, g, b);
	if (!ok) {
		gamma_control_send_failed(gamma_control);
		goto error_table;
	}

	free(table);
	return;

error_table:
	free(table);
error_fd:
	close(fd);
}

static const struct zwlr_gamma_control_v1_interface gamma_control_impl = {
	.destroy = gamma_control_handle_destroy,
	.set_gamma = gamma_control_handle_set_gamma,
};

static const struct zwlr_gamma_control_manager_v1_interface
	gamma_control_manager_impl;

static struct wlr_gamma_control_manager_v1 *gamma_control_manager_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&zwlr_gamma_control_manager_v1_interface, &gamma_control_manager_impl));
	return wl_resource_get_user_data(resource);
}

static void gamma_control_manager_get_gamma_control(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		struct wl_resource *output_resource) {
	struct wlr_gamma_control_manager_v1 *manager =
		gamma_control_manager_from_resource(manager_resource);
	struct wlr_output *output = wlr_output_from_resource(output_resource);

	struct wlr_gamma_control_v1 *gamma_control =
		calloc(1, sizeof(struct wlr_gamma_control_v1));
	if (gamma_control == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	gamma_control->output = output;

	uint32_t version = wl_resource_get_version(manager_resource);
	gamma_control->resource = wl_resource_create(client,
		&zwlr_gamma_control_v1_interface, version, id);
	if (gamma_control->resource == NULL) {
		free(gamma_control);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(gamma_control->resource, &gamma_control_impl,
		gamma_control, gamma_control_handle_resource_destroy);

	wl_signal_add(&output->events.destroy,
		&gamma_control->output_destroy_listener);
	gamma_control->output_destroy_listener.notify =
		gamma_control_handle_output_destroy;

	wl_list_init(&gamma_control->link);

	if (!output->impl->set_gamma) {
		zwlr_gamma_control_v1_send_failed(gamma_control->resource);
		gamma_control_destroy(gamma_control);
		return;
	}

	struct wlr_gamma_control_v1 *gc;
	wl_list_for_each(gc, &manager->controls, link) {
		if (gc->output == output) {
			zwlr_gamma_control_v1_send_failed(gc->resource);
			gamma_control_destroy(gc);
			return;
		}
	}

	wl_list_remove(&gamma_control->link);
	wl_list_insert(&manager->controls, &gamma_control->link);
	zwlr_gamma_control_v1_send_gamma_size(gamma_control->resource,
		wlr_output_get_gamma_size(output));
}

static const struct zwlr_gamma_control_manager_v1_interface
		gamma_control_manager_impl = {
	.get_gamma_control = gamma_control_manager_get_gamma_control,
};

static void gamma_control_manager_handle_resource_destroy(
		struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

static void gamma_control_manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wlr_gamma_control_manager_v1 *manager = data;

	struct wl_resource *resource = wl_resource_create(client,
		&zwlr_gamma_control_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &gamma_control_manager_impl,
		manager, gamma_control_manager_handle_resource_destroy);
	wl_list_insert(&manager->resources, wl_resource_get_link(resource));
}

void wlr_gamma_control_manager_v1_destroy(
		struct wlr_gamma_control_manager_v1 *manager) {
	if (!manager) {
		return;
	}
	wl_list_remove(&manager->display_destroy.link);
	struct wlr_gamma_control_v1 *gamma_control, *tmp;
	wl_list_for_each_safe(gamma_control, tmp, &manager->controls, link) {
		wl_resource_destroy(gamma_control->resource);
	}
	struct wl_resource *resource, *resource_tmp;
	wl_resource_for_each_safe(resource, resource_tmp, &manager->resources) {
		wl_resource_destroy(resource);
	}
	wl_global_destroy(manager->global);
	free(manager);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_gamma_control_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);
	wlr_gamma_control_manager_v1_destroy(manager);
}

struct wlr_gamma_control_manager_v1 *wlr_gamma_control_manager_v1_create(
		struct wl_display *display) {
	struct wlr_gamma_control_manager_v1 *manager =
		calloc(1, sizeof(struct wlr_gamma_control_manager_v1));
	if (!manager) {
		return NULL;
	}

	manager->global = wl_global_create(display,
		&zwlr_gamma_control_manager_v1_interface,
		GAMMA_CONTROL_MANAGER_V1_VERSION, manager, gamma_control_manager_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}

	wl_list_init(&manager->resources);
	wl_list_init(&manager->controls);

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}
