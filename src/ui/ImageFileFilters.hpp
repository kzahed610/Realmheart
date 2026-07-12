#pragma once

#include <gio/gio.h>

namespace realmheart::ui {

// Returns a new GListModel reference. The caller must unref it.
GListModel* create_image_file_filters();

} // namespace realmheart::ui
