#include "ui/ImageFileFilters.hpp"

#include <gtk/gtk.h>

namespace realmheart::ui {

GListModel* create_image_file_filters() {
    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Images");
    gtk_file_filter_add_mime_type(filter, "image/jpeg");
    gtk_file_filter_add_mime_type(filter, "image/png");
    gtk_file_filter_add_mime_type(filter, "image/webp");
    gtk_file_filter_add_mime_type(filter, "image/avif");
    gtk_file_filter_add_mime_type(filter, "image/bmp");
    gtk_file_filter_add_mime_type(filter, "image/tiff");

    GListStore* store = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(store, filter);
    g_object_unref(filter);
    return G_LIST_MODEL(store);
}

} // namespace realmheart::ui
