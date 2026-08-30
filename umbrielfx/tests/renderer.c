#include <dirent.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <xf86drm.h>

#include "umbrielfx/render/fx_renderer/fx_renderer.h"

#define CREATE_CYCLES 8

static bool same_drm_device(int first_fd, int second_fd) {
	drmDevice *first = NULL;
	drmDevice *second = NULL;
	bool same = drmGetDevice2(first_fd, 0, &first) == 0 &&
		drmGetDevice2(second_fd, 0, &second) == 0 &&
		drmDevicesEqual(first, second);
	drmFreeDevice(&first);
	drmFreeDevice(&second);
	return same;
}

static int count_open_fds(void) {
	DIR *directory = opendir("/proc/self/fd");
	if (directory == NULL) {
		return -1;
	}

	int count = 0;
	struct dirent *entry;
	while ((entry = readdir(directory)) != NULL) {
		if (strcmp(entry->d_name, ".") != 0 &&
				strcmp(entry->d_name, "..") != 0) {
			count++;
		}
	}
	closedir(directory);
	return count;
}

static void print_open_fds(void) {
	DIR *directory = opendir("/proc/self/fd");
	if (directory == NULL) {
		return;
	}
	struct dirent *entry;
	while ((entry = readdir(directory)) != NULL) {
		if (entry->d_name[0] == '.') {
			continue;
		}
		char target[256];
		ssize_t length = readlinkat(dirfd(directory), entry->d_name,
			target, sizeof(target) - 1);
		if (length >= 0) {
			target[length] = '\0';
			fprintf(stderr, "  fd %s: %s\n", entry->d_name, target);
		}
	}
	closedir(directory);
}

static struct wlr_renderer *create_owned_renderer(int drm_fd) {
	(void)drm_fd;
	return fx_renderer_create(NULL);
}

static bool renderer_matches(struct wlr_renderer *renderer, int drm_fd) {
	int renderer_fd = wlr_renderer_get_drm_fd(renderer);
	return renderer_fd >= 0 && same_drm_device(drm_fd, renderer_fd);
}

static bool renderer_is_usable(struct wlr_renderer *renderer) {
	const uint32_t input = 0xFF224466;
	uint32_t output = 0;
	struct wlr_texture *texture = wlr_texture_from_pixels(renderer,
		DRM_FORMAT_ARGB8888, sizeof(input), 1, 1, &input);
	if (texture == NULL) {
		return false;
	}
	bool ok = wlr_texture_read_pixels(texture,
		&(struct wlr_texture_read_pixels_options) {
			.data = &output,
			.format = DRM_FORMAT_ARGB8888,
			.stride = sizeof(output),
		});
	wlr_texture_destroy(texture);
	return ok && output == input;
}

static bool replacement_survives_old_renderer_destroy(int drm_fd) {
	struct wlr_renderer *old_renderer =
		fx_renderer_create_with_drm_fd_gbm(drm_fd);
	struct wlr_renderer *replacement =
		fx_renderer_create_with_drm_fd_gbm(drm_fd);
	if (old_renderer == NULL || replacement == NULL) {
		if (old_renderer != NULL) {
			wlr_renderer_destroy(old_renderer);
		}
		if (replacement != NULL) {
			wlr_renderer_destroy(replacement);
		}
		return false;
	}

	wlr_renderer_destroy(old_renderer);
	bool ok = renderer_matches(replacement, drm_fd) &&
		renderer_is_usable(replacement);
	wlr_renderer_destroy(replacement);
	return ok;
}

static bool descriptors_are_stable(
		int drm_fd, struct wlr_renderer *(*create_renderer)(int)) {
	struct wlr_renderer *renderer = create_renderer(drm_fd);
	if (renderer == NULL) {
		fprintf(stderr, "FAIL: renderer creation failed\n");
		return false;
	}
	if (!renderer_matches(renderer, drm_fd)) {
		fprintf(stderr, "FAIL: renderer used a different DRM device\n");
		wlr_renderer_destroy(renderer);
		return false;
	}
	if (!renderer_is_usable(renderer)) {
		fprintf(stderr, "FAIL: renderer could not upload and read back a texture\n");
		wlr_renderer_destroy(renderer);
		return false;
	}
	wlr_renderer_destroy(renderer);

	int before = count_open_fds();
	if (before < 0) {
		fprintf(stderr, "FAIL: could not inspect /proc/self/fd\n");
		return false;
	}
	for (int i = 0; i < CREATE_CYCLES; i++) {
		renderer = create_renderer(drm_fd);
		if (renderer == NULL) {
			fprintf(stderr, "FAIL: renderer creation failed in cycle %d\n", i + 1);
			return false;
		}
		if (!renderer_matches(renderer, drm_fd)) {
			fprintf(stderr, "FAIL: renderer changed DRM device in cycle %d\n", i + 1);
			wlr_renderer_destroy(renderer);
			return false;
		}
		wlr_renderer_destroy(renderer);
	}
	int after = count_open_fds();
	if (after != before) {
		fprintf(stderr, "FAIL: open descriptor count changed from %d to %d\n",
			before, after);
		print_open_fds();
		return false;
	}
	return true;
}

int main(void) {
	drmDevice **devices = NULL;
	int devices_len = drmGetDevices2(0, NULL, 0);
	if (devices_len <= 0) {
		fprintf(stderr, "SKIP: no DRM devices\n");
		return 77;
	}
	devices = calloc(devices_len, sizeof(*devices));
	if (devices == NULL) {
		return EXIT_FAILURE;
	}
	devices_len = drmGetDevices2(0, devices, devices_len);
	if (devices_len < 0) {
		free(devices);
		return EXIT_FAILURE;
	}

	bool tested = false;
	bool passed = false;
	for (int i = 0; i < devices_len && !tested; i++) {
		if (!(devices[i]->available_nodes & (1 << DRM_NODE_RENDER))) {
			continue;
		}
		const char *path = devices[i]->nodes[DRM_NODE_RENDER];
		int drm_fd = open(path, O_RDWR | O_CLOEXEC);
		if (drm_fd < 0) {
			continue;
		}
		if (setenv("WLR_RENDER_DRM_DEVICE", path, 1) != 0) {
			tested = true;
			close(drm_fd);
			break;
		}

		struct wlr_renderer *probe =
			fx_renderer_create_with_drm_fd_gbm(drm_fd);
		if (probe == NULL) {
			close(drm_fd);
			continue;
		}
		tested = true;
		if (!renderer_matches(probe, drm_fd)) {
			wlr_renderer_destroy(probe);
			close(drm_fd);
			break;
		}
		wlr_renderer_destroy(probe);

		if (!replacement_survives_old_renderer_destroy(drm_fd)) {
			fprintf(stderr,
				"FAIL: replacement renderer did not survive old renderer teardown\n");
		} else if (descriptors_are_stable(drm_fd,
				fx_renderer_create_with_drm_fd_gbm)) {
			passed = descriptors_are_stable(drm_fd, create_owned_renderer);
		}
		close(drm_fd);
	}

	drmFreeDevices(devices, devices_len);
	free(devices);
	if (!tested) {
		fprintf(stderr, "SKIP: no usable DRM render node\n");
		return 77;
	}
	if (!passed) {
		fprintf(stderr, "FAIL: renderer changed GPU or leaked a descriptor\n");
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
