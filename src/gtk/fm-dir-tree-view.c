//      fm-dir-tree-view.c
//
//      Copyright 2011 Hong Jen Yee (PCMan) <pcman.tw@gmail.com>
//
//      This program is free software; you can redistribute it and/or modify
//      it under the terms of the GNU General Public License as published by
//      the Free Software Foundation; either version 2 of the License, or
//      (at your option) any later version.
//
//      This program is distributed in the hope that it will be useful,
//      but WITHOUT ANY WARRANTY; without even the implied warranty of
//      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//      GNU General Public License for more details.
//
//      You should have received a copy of the GNU General Public License
//      along with this program; if not, write to the Free Software
//      Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
//      MA 02110-1301, USA.

#include "fm-dir-tree-view.h"
#include "fm-dir-tree-model.h"
#include "../gtk-compat.h"
#include <gdk/gdkkeysyms.h>
#include <string.h>

enum
{
    CHDIR,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static void fm_dir_tree_view_finalize            (GObject *object);
static void on_folder_loaded(FmFolder* folder, FmDirTreeView* view);

G_DEFINE_TYPE(FmDirTreeView, fm_dir_tree_view, GTK_TYPE_TREE_VIEW)

static inline FmFolder* fm_dir_tree_view_get_folder(GtkTreeRowReference* ref)
{
    GtkTreeModel* model;
    GtkTreePath* path;
    GtkTreeIter it;
    GValue val = G_VALUE_INIT;

    g_return_val_if_fail(ref != NULL, NULL);
    model = gtk_tree_row_reference_get_model(ref);
    path = gtk_tree_row_reference_get_path(ref);
    if(G_UNLIKELY(path == NULL || !gtk_tree_model_get_iter(model, &it, path)))
    {
        gtk_tree_path_free(path);
        return NULL;
    }
    gtk_tree_path_free(path);
    gtk_tree_model_get_value(model, &it, FM_DIR_TREE_MODEL_COL_FOLDER, &val);
    return (FmFolder*)g_value_get_pointer(&val);
}

static void cancel_pending_chdir(FmDirTreeView *view)
{
    if(view->paths_to_expand)
    {
        if(G_LIKELY(view->current_row)) /* FIXME: else data are corrupted */
        {
            FmFolder* folder = fm_dir_tree_view_get_folder(view->current_row);

            if(G_LIKELY(folder))
                g_signal_handlers_disconnect_by_func(folder, on_folder_loaded, view);
            gtk_tree_row_reference_free(view->current_row);
            view->current_row = NULL;
        }
        g_slist_foreach(view->paths_to_expand, (GFunc)fm_path_unref, NULL);
        g_slist_free(view->paths_to_expand);
        view->paths_to_expand = NULL;
    }
}

static gboolean on_test_expand_row(GtkTreeView *tree_view, GtkTreeIter *iter, GtkTreePath *path)
{
    FmDirTreeModel* model = FM_DIR_TREE_MODEL(gtk_tree_view_get_model(tree_view));

    fm_dir_tree_model_expand_row(model, iter, path);

    return FALSE;
}

static gboolean find_iter_by_path(GtkTreeModel* model, GtkTreeIter* it, GtkTreePath* tp, FmPath* path)
{
    GtkTreeIter parent;

    gtk_tree_model_get_iter(model, &parent, tp);
    if(gtk_tree_model_iter_children(model, it, &parent))
    {
        do{
            FmPath* path2 = NULL;
            gtk_tree_model_get(model, it, FM_DIR_TREE_MODEL_COL_PATH, &path2, -1);
            if(path2 && fm_path_equal(path, path2))
                return TRUE;
        }while(gtk_tree_model_iter_next(model, it));
    }
    return FALSE;
}

/*
static void on_row_expanded(GtkTreeView *tree_view, GtkTreeIter *iter, GtkTreePath *path)
{
    FmDirTreeView* view = FM_DIR_TREE_VIEW(tree_view);
}
*/

static void on_row_collapsed(GtkTreeView *tree_view, GtkTreeIter *iter, GtkTreePath *path)
{
    FmDirTreeModel* model = FM_DIR_TREE_MODEL(gtk_tree_view_get_model(tree_view));
    fm_dir_tree_model_collapse_row(model, iter, path);
}

static void on_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *col)
{
    if(gtk_tree_view_row_expanded(tree_view, path))
        gtk_tree_view_collapse_row(tree_view, path);
    else
        gtk_tree_view_expand_row(tree_view, path, FALSE);
}

static gboolean on_key_press_event(GtkWidget* widget, GdkEventKey* evt)
{
    GtkTreeView* tree_view = GTK_TREE_VIEW(widget);
    GtkTreeSelection* tree_sel;
    GtkTreeModel* model;
    GtkTreeIter it;
    GtkTreePath* tp;

    switch(evt->keyval)
    {
    case GDK_KEY_Left:
        tree_sel = gtk_tree_view_get_selection(tree_view);
        if(gtk_tree_selection_get_selected(tree_sel, &model, &it))
        {
            tp = gtk_tree_model_get_path(model, &it);
            if(gtk_tree_view_row_expanded(tree_view, tp))
                gtk_tree_view_collapse_row(tree_view, tp);
            else
            {
                gtk_tree_path_up(tp);
                gtk_tree_view_set_cursor(tree_view, tp, NULL, FALSE);
                gtk_tree_selection_select_path(tree_sel, tp);
            }
            gtk_tree_path_free(tp);
        }

        break;
    case GDK_KEY_Right:
        tree_sel = gtk_tree_view_get_selection(tree_view);
        if(gtk_tree_selection_get_selected(tree_sel, &model, &it))
        {
            tp = gtk_tree_model_get_path(model, &it);
            gtk_tree_view_expand_row(tree_view, tp, FALSE);
            gtk_tree_path_free(tp);
        }
        break;
    }
    return GTK_WIDGET_CLASS(fm_dir_tree_view_parent_class)->key_press_event(widget, evt);
}

static void fm_dir_tree_view_class_init(FmDirTreeViewClass *klass)
{
    GObjectClass *g_object_class = G_OBJECT_CLASS(klass);
    GtkTreeViewClass* tree_view_class = GTK_TREE_VIEW_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    g_object_class->finalize = fm_dir_tree_view_finalize;

    widget_class->key_press_event = on_key_press_event;
    // widget_class->button_press_event = on_button_press_event;

    tree_view_class->test_expand_row = on_test_expand_row;
    tree_view_class->row_collapsed = on_row_collapsed;
    /* tree_view_class->row_expanded = on_row_expanded; */
    tree_view_class->row_activated = on_row_activated;

    signals[CHDIR] =
        g_signal_new("chdir",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     G_STRUCT_OFFSET(FmDirTreeViewClass, chdir),
                     NULL, NULL,
                     g_cclosure_marshal_VOID__UINT_POINTER,
                     G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_POINTER);
}

static void emit_chdir_if_needed(FmDirTreeView* view, GtkTreeSelection* tree_sel, guint button)
{
    GtkTreeIter it;
    GtkTreeModel* model;
    if(gtk_tree_selection_get_selected(tree_sel, &model, &it))
    {
        FmPath* path;
        gtk_tree_model_get(model, &it, FM_DIR_TREE_MODEL_COL_PATH, &path, -1);
        if(path && view->cwd && fm_path_equal(path, view->cwd))
            return;
        if(view->cwd)
            fm_path_unref(view->cwd);
        view->cwd = G_LIKELY(path) ? fm_path_ref(path) : NULL;
        g_signal_emit(view, signals[CHDIR], 0, button, view->cwd);
    }
}

static void on_sel_changed(GtkTreeSelection* tree_sel, FmDirTreeView* view)
{
    /* if a pending selection via previous call to chdir is in progress, cancel it. */
    cancel_pending_chdir(view);

    emit_chdir_if_needed(view, tree_sel, 1);
}

static void fm_dir_tree_view_finalize(GObject *object)
{
    FmDirTreeView *view;

    g_return_if_fail(object != NULL);
    g_return_if_fail(FM_IS_DIR_TREE_VIEW(object));

    view = (FmDirTreeView*)object;
    if(G_UNLIKELY(view->paths_to_expand))
        cancel_pending_chdir(view);
    gtk_tree_row_reference_free(view->current_row);

    if(view->cwd)
        fm_path_unref(view->cwd);

    G_OBJECT_CLASS(fm_dir_tree_view_parent_class)->finalize(object);
}

static gboolean _fm_dir_tree_view_select_function(GtkTreeSelection *selection,
                                                  GtkTreeModel *model,
                                                  GtkTreePath *path,
                                                  gboolean path_currently_selected,
                                                  gpointer data)
{
    GtkTreeIter it;
    GValue val = G_VALUE_INIT;

    if(!gtk_tree_model_get_iter(model, &it, path))
        return FALSE;
    gtk_tree_model_get_value(model, &it, FM_DIR_TREE_MODEL_COL_INFO, &val);
    return (g_value_get_pointer(&val) != NULL);
}

static void fm_dir_tree_view_init(FmDirTreeView *view)
{
    GtkTreeSelection* tree_sel;
    GtkTreeViewColumn* col;
    GtkCellRenderer* render;
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(view), FALSE);
    /* gtk_tree_view_set_enable_tree_lines(view, TRUE); */

    col = gtk_tree_view_column_new();
    render = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(col, render, FALSE);
    gtk_tree_view_column_set_attributes(col, render, "pixbuf", FM_DIR_TREE_MODEL_COL_ICON, NULL);

    render = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, render, TRUE);
    gtk_tree_view_column_set_attributes(col, render, "text", FM_DIR_TREE_MODEL_COL_DISP_NAME, NULL);

    gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);

    tree_sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
    gtk_tree_selection_set_mode(tree_sel, GTK_SELECTION_BROWSE);
    gtk_tree_selection_set_select_function(tree_sel,
                        _fm_dir_tree_view_select_function, view, NULL);
    g_signal_connect(tree_sel, "changed", G_CALLBACK(on_sel_changed), view);
}


FmDirTreeView *fm_dir_tree_view_new(void)
{
    return g_object_new(FM_TYPE_DIR_TREE_VIEW, NULL);
}

FmPath* fm_dir_tree_view_get_cwd(FmDirTreeView* view)
{
    return view->cwd;
}

static void expand_pending_path(FmDirTreeView* view, GtkTreeModel* model, GtkTreePath* parent);

static void on_folder_loaded(FmFolder* folder, FmDirTreeView* view)
{
    FmPath* path;
    GtkTreeModel* model;
    GtkTreePath* tp;

    g_return_if_fail(view->current_row);

    /* disconnect the handler since we only need it once */
    g_signal_handlers_disconnect_by_func(folder, on_folder_loaded, view);

    /* after the folder is loaded, the files should have been added to
     * the tree model */
    model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
    tp = gtk_tree_row_reference_get_path(view->current_row);
    gtk_tree_view_expand_row(GTK_TREE_VIEW(view), tp, FALSE);

    /* remove the expanded path from pending list */
    path = FM_PATH(view->paths_to_expand->data);
    view->paths_to_expand = g_slist_delete_link(view->paths_to_expand, view->paths_to_expand);

    if(G_UNLIKELY(!tp))
        ; /* it must be a bug */
    else if(view->paths_to_expand)
    {
        /* continue expanding next pending path */
        expand_pending_path(view, model, tp);
    }
    else /* this is the last one and we're done, select the item */
    {
        GtkTreeSelection* ts = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
        gtk_tree_selection_select_path(ts, tp);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(view), tp, NULL, TRUE, 0.5, 0.5);
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(view), tp, NULL, FALSE);
    }
    gtk_tree_path_free(tp);
    fm_path_unref(path);
}

static void expand_pending_path(FmDirTreeView* view, GtkTreeModel* model, GtkTreePath* tp)
{
    FmPath* path;
    GtkTreeIter it;
    g_return_if_fail(view->paths_to_expand);
    path = FM_PATH(view->paths_to_expand->data);

    if(find_iter_by_path(model, &it, tp, path))
    {
        FmFolder* folder;

        gtk_tree_row_reference_free(view->current_row);

        /* after being expanded, the row now owns a FmFolder object. */
        gtk_tree_model_get(model, &it, FM_DIR_TREE_MODEL_COL_FOLDER, &folder, -1);
        /* FIXME: can it fail now? */

        if(fm_folder_is_loaded(folder)) /* the folder is already loaded */
            on_folder_loaded(folder, view);
        else /* the folder is not yet loaded */
            g_signal_connect(folder, "finish-loading", G_CALLBACK(on_folder_loaded), view);

        tp = gtk_tree_model_get_path(model, &it); /* it now points to the item */
        view->current_row = gtk_tree_row_reference_new(model, tp);
        gtk_tree_path_free(tp);
    }
    /* FIXME: otherwise it's a bug */
}

void fm_dir_tree_view_chdir(FmDirTreeView* view, FmPath* path)
{
    GtkTreeIter it;
    GtkTreeModel* model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
    FmPath* root;
    if(!model || fm_path_equal(view->cwd, path))
        return;
    if(!gtk_tree_model_get_iter_first(model, &it))
        return;

    /* find a root item containing this path */
    do{
        gtk_tree_model_get(model, &it, FM_DIR_TREE_MODEL_COL_PATH, &root, -1);
        if(fm_path_has_prefix(path, root))
            break;
        root = NULL;
    }while(gtk_tree_model_iter_next(model, &it));
    /* root item is found */

    /* cancel previous pending tree expansion */
    cancel_pending_chdir(view);

    do { /* add path elements one by one to a list */
        view->paths_to_expand = g_slist_prepend(view->paths_to_expand, fm_path_ref(path));
        if(fm_path_equal(path, root))
            break;
        path = path->parent;
    }while(path);

    expand_pending_path(view, model, NULL);
}
