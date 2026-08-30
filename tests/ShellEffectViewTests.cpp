#include "effects/shell/ShellEffectView.hpp"

#include <cassert>
#include <iostream>

namespace {

typedef struct _SnapshotProbe SnapshotProbe;
typedef struct _SnapshotProbeClass SnapshotProbeClass;

struct _SnapshotProbe {
    GtkWidget parent_instance;
};

struct _SnapshotProbeClass {
    GtkWidgetClass parent_class;
};

G_DEFINE_TYPE(SnapshotProbe, snapshot_probe, GTK_TYPE_WIDGET)

void snapshot_probe_measure(
    GtkWidget* /*widget*/,
    GtkOrientation /*orientation*/,
    int /*for_size*/,
    int* minimum,
    int* natural,
    int* minimum_baseline,
    int* natural_baseline
) {
    if (minimum != nullptr) *minimum = 1;
    if (natural != nullptr) *natural = 1;
    if (minimum_baseline != nullptr) *minimum_baseline = -1;
    if (natural_baseline != nullptr) *natural_baseline = -1;
}

void snapshot_probe_snapshot(GtkWidget* /*widget*/, GtkSnapshot* snapshot) {
    const graphene_rect_t bounds = GRAPHENE_RECT_INIT(0.0F, 0.0F, 1.0F, 1.0F);
    const GdkRGBA color = {1.0, 0.0, 0.0, 1.0};
    gtk_snapshot_append_color(snapshot, &color, &bounds);
}

void snapshot_probe_class_init(SnapshotProbeClass* klass) {
    auto* widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->measure = snapshot_probe_measure;
    widget_class->snapshot = snapshot_probe_snapshot;
}

void snapshot_probe_init(SnapshotProbe* /*probe*/) {}

} // namespace

int main() {
    gtk_init();

    auto* probe = GTK_WIDGET(g_object_new(snapshot_probe_get_type(), nullptr));
    auto* view = realmheart_shell_effect_view_new(probe);
    auto* root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    auto* window = gtk_window_new();

    gtk_box_append(GTK_BOX(root), view);
    gtk_widget_set_visible(root, TRUE);
    gtk_widget_set_visible(view, TRUE);
    gtk_widget_set_visible(probe, TRUE);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(window), 1, 1);
    gtk_window_set_child(GTK_WINDOW(window), root);
    gtk_window_present(GTK_WINDOW(window));

    for (int iteration = 0; iteration < 20; ++iteration) {
        while (g_main_context_pending(nullptr)) {
            g_main_context_iteration(nullptr, FALSE);
        }
        if (gtk_widget_get_mapped(view)) break;
    }
    assert(gtk_widget_get_mapped(view));

    auto* snapshot = gtk_snapshot_new();
    gtk_widget_snapshot_child(GTK_WIDGET(window), root, snapshot);
    auto* node = gtk_snapshot_free_to_node(snapshot);

    assert(node != nullptr);
    gsk_render_node_unref(node);
    g_object_ref(window);
    gtk_window_destroy(GTK_WINDOW(window));
    g_object_unref(window);

    std::cout << "Shell effect view tests passed\n";
    return 0;
}
