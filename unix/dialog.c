/*
 * dialog.c - GTK implementation of the PuTTY configuration box.
 */

#include <assert.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>

#include <gtk/gtk.h>
#if !GTK_CHECK_VERSION(3,0,0)
#include <gdk/gdkkeysyms.h>
#endif

#define MAY_REFER_TO_GTK_IN_HEADERS

#include "putty.h"
#include "gtkcompat.h"
#include "columns.h"
#include "unifont.h"
#include "gtkmisc.h"

#ifndef NOT_X_WINDOWS
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "x11misc.h"
#endif

#include "storage.h"
#include "dialog.h"
#include "tree234.h"
#include "licence.h"
#include "ssh.h"

#if GTK_CHECK_VERSION(2,0,0)
/* Decide which of GtkFileChooserDialog and GtkFileSelection to use */
#define USE_GTK_FILE_CHOOSER_DIALOG
#endif

struct Shortcut {
    GtkWidget *widget;
    struct uctrl *uc;
    int action;
};

struct Shortcuts {
    struct Shortcut sc[128];
};

struct selparam;

struct uctrl {
    dlgcontrol *ctrl;
    GtkWidget *toplevel;
    GtkWidget **buttons; int nbuttons; /* for radio buttons */
    GtkWidget *entry;         /* for editbox, filesel, fontsel */
    GtkWidget *button;        /* for filesel, fontsel */
#if !GTK_CHECK_VERSION(2,4,0)
    GtkWidget *list;          /* for listbox (in GTK1), combobox (<=GTK2.3) */
    GtkWidget *menu;          /* for optionmenu (==droplist) */
    GtkWidget *optmenu;       /* also for optionmenu */
#else
    GtkWidget *combo;         /* for combo box (either editable or not) */
#endif
#if GTK_CHECK_VERSION(2,0,0)
    GtkWidget *treeview;      /* for listbox (GTK2), droplist+combo (>=2.4) */
    GtkListStore *listmodel;  /* for all types of list box */
#endif
    GtkWidget *text;          /* for text */
    GtkWidget *label;         /* for dlg_label_change */
    GtkAdjustment *adj;       /* for the scrollbar in a list box */
    struct selparam *sp;      /* which switchable pane of the box we're in */
    guint textsig;
    int nclicks;
    const char *textvalue;    /* temporary, for button-only file selectors */
};

struct dlgparam {
    tree234 *byctrl, *bywidget;
    void *data;
    struct {
        unsigned char r, g, b;         /* 0-255 */
        bool ok;
    } coloursel_result;
    /* `flags' are set to indicate when a GTK signal handler is being called
     * due to automatic processing and should not flag a user event. */
    int flags;
    struct Shortcuts *shortcuts;
    GtkWidget *window, *cancelbutton;
    dlgcontrol *currfocus, *lastfocus;
#if !GTK_CHECK_VERSION(2,0,0)
    GtkWidget *currtreeitem, **treeitems;
    int ntreeitems;
#else
    size_t nselparams;
    struct selparam **selparams;
#endif
    struct selparam *curr_panel;
    struct controlbox *ctrlbox;
    int retval;
    post_dialog_fn_t after;
    void *afterctx;
};
#define FLAG_UPDATING_COMBO_LIST 1
#define FLAG_UPDATING_LISTBOX    2

enum {                                 /* values for Shortcut.action */
    SHORTCUT_EMPTY,                    /* no shortcut on this key */
    SHORTCUT_TREE,                     /* focus a tree item */
    SHORTCUT_FOCUS,                    /* focus the supplied widget */
    SHORTCUT_UCTRL,                    /* do something sane with uctrl */
    SHORTCUT_UCTRL_UP,                 /* uctrl is a draglist, move Up */
    SHORTCUT_UCTRL_DOWN,               /* uctrl is a draglist, move Down */
};

#if GTK_CHECK_VERSION(2,0,0)
enum {
    TREESTORE_PATH,
    TREESTORE_PARAMS,
    TREESTORE_NUM
};
#endif

/*
 * Forward references.
 */
static gboolean widget_focus(GtkWidget *widget, GdkEventFocus *event,
                             gpointer data);
static void shortcut_add(struct Shortcuts *scs, GtkWidget *labelw,
                         int chr, int action, void *ptr);
static void shortcut_highlight(GtkWidget *label, int chr);
#if !GTK_CHECK_VERSION(2,0,0)
static gboolean listitem_single_key(GtkWidget *item, GdkEventKey *event,
                                    gpointer data);
static gboolean listitem_multi_key(GtkWidget *item, GdkEventKey *event,
                                   gpointer data);
static gboolean listitem_button_press(GtkWidget *item, GdkEventButton *event,
                                      gpointer data);
static gboolean listitem_button_release(GtkWidget *item, GdkEventButton *event,
                                        gpointer data);
#endif
#if !GTK_CHECK_VERSION(2,4,0)
static void menuitem_activate(GtkMenuItem *item, gpointer data);
#endif
#if GTK_CHECK_VERSION(3,0,0)
static void colourchoose_response(GtkDialog *dialog,
                                  gint response_id, gpointer data);
#else
static void coloursel_ok(GtkButton *button, gpointer data);
static void coloursel_cancel(GtkButton *button, gpointer data);
#endif
static void dlgparam_destroy(GtkWidget *widget, gpointer data);
static int get_listitemheight(GtkWidget *widget);

static int uctrl_cmp_byctrl(void *av, void *bv)
{
    struct uctrl *a = (struct uctrl *)av;
    struct uctrl *b = (struct uctrl *)bv;
    if (a->ctrl < b->ctrl)
        return -1;
    else if (a->ctrl > b->ctrl)
        return +1;
    return 0;
}

static int uctrl_cmp_byctrl_find(void *av, void *bv)
{
    dlgcontrol *a = (dlgcontrol *)av;
    struct uctrl *b = (struct uctrl *)bv;
    if (a < b->ctrl)
        return -1;
    else if (a > b->ctrl)
        return +1;
    return 0;
}

static int uctrl_cmp_bywidget(void *av, void *bv)
{
    struct uctrl *a = (struct uctrl *)av;
    struct uctrl *b = (struct uctrl *)bv;
    if (a->toplevel < b->toplevel)
        return -1;
    else if (a->toplevel > b->toplevel)
        return +1;
    return 0;
}

static int uctrl_cmp_bywidget_find(void *av, void *bv)
{
    GtkWidget *a = (GtkWidget *)av;
    struct uctrl *b = (struct uctrl *)bv;
    if (a < b->toplevel)
        return -1;
    else if (a > b->toplevel)
        return +1;
    return 0;
}

static void dlg_init(struct dlgparam *dp)
{
    dp->byctrl = newtree234(uctrl_cmp_byctrl);
    dp->bywidget = newtree234(uctrl_cmp_bywidget);
    dp->coloursel_result.ok = false;
    dp->window = dp->cancelbutton = NULL;
#if !GTK_CHECK_VERSION(2,0,0)
    dp->treeitems = NULL;
    dp->currtreeitem = NULL;
#endif
    dp->curr_panel = NULL;
    dp->flags = 0;
    dp->currfocus = NULL;
}

static void dlg_cleanup(struct dlgparam *dp)
{
    struct uctrl *uc;

    freetree234(dp->byctrl);           /* doesn't free the uctrls inside */
    dp->byctrl = NULL;
    while ( (uc = index234(dp->bywidget, 0)) != NULL) {
        del234(dp->bywidget, uc);
        sfree(uc->buttons);
        sfree(uc);
    }
    freetree234(dp->bywidget);
    dp->bywidget = NULL;
#if !GTK_CHECK_VERSION(2,0,0)
    sfree(dp->treeitems);
#endif
}

static void dlg_add_uctrl(struct dlgparam *dp, struct uctrl *uc)
{
    add234(dp->byctrl, uc);
    add234(dp->bywidget, uc);
}

static struct uctrl *dlg_find_byctrl(struct dlgparam *dp, dlgcontrol *ctrl)
{
    if (!dp->byctrl)
        return NULL;
    return find234(dp->byctrl, ctrl, uctrl_cmp_byctrl_find);
}

static struct uctrl *dlg_find_bywidget(struct dlgparam *dp, GtkWidget *w)
{
    struct uctrl *ret = NULL;
    if (!dp->bywidget)
        return NULL;
    do {
        ret = find234(dp->bywidget, w, uctrl_cmp_bywidget_find);
        if (ret)
            return ret;
        w = gtk_widget_get_parent(w);
    } while (w);
    return ret;
}

dlgcontrol *dlg_last_focused(dlgcontrol *ctrl, dlgparam *dp)
{
    if (dp->currfocus != ctrl)
        return dp->currfocus;
    else
        return dp->lastfocus;
}

void dlg_radiobutton_set(dlgcontrol *ctrl, dlgparam *dp, int which)
{
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    assert(uc->ctrl->type == CTRL_RADIO);
    assert(uc->buttons != NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(uc->buttons[which]), true);
}

int dlg_radiobutton_get(dlgcontrol *ctrl, dlgparam *dp)
{
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    int i;

    assert(uc->ctrl->type == CTRL_RADIO);
    assert(uc->buttons != NULL);
    for (i = 0; i < uc->nbuttons; i++)
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(uc->buttons[i])))
            return i;
    return 0;                          /* got to return something */
}

void dlg_checkbox_set(dlgcontrol *ctrl, dlgparam *dp, bool checked)
{
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    assert(uc->ctrl->type == CTRL_CHECKBOX);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(uc->toplevel), checked);
}

bool dlg_checkbox_get(dlgcontrol *ctrl, dlgparam *dp)
{
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    assert(uc->ctrl->type == CTRL_CHECKBOX);
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(uc->toplevel));
}

void dlg_editbox_set_utf8(dlgcontrol *ctrl, dlgparam *dp, char const *text)
{
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    GtkWidget *entry;
    char *tmpstring;
    assert(uc->ctrl->type == CTRL_EDITBOX);

#if GTK_CHECK_VERSION(2,4,0)
    if (uc->combo)
        entry = gtk_bin_get_child(GTK_BIN(uc->combo));
    else
#endif
        entry = uc->entry;

    assert(entry != NULL);

    /*
     * GTK 2 implements gtk_entry_set_text by means of two separate
     * operations: first delete the previous text leaving the empty
     * string, then insert the new text. This causes two calls to
     * the "changed" signal.
     *
     * The first call to "changed", if allowed to proceed normally,
     * will cause an EVENT_VALCHANGE event on the edit box, causing
     * a call to dlg_editbox_get() which will read the empty string
     * out of the GtkEntry - and promptly write it straight into the
     * Conf structure, which is precisely where our `text' pointer
     * is probably pointing, so the second editing operation will
     * insert that instead of the string we originally asked for.
     *
     * Hence, we must take our own copy of the text before we do
     * this.
     */
    tmpstring = dupstr(text);
    gtk_entry_set_text(GTK_ENTRY(entry), tmpstring);
    sfree(tmpstring);
}

void dlg_editbox_set(dlgcontrol *ctrl, dlgparam *dp, char const *text)
{
    /* GTK specifies that its edit boxes are always in UTF-8 anyway,
     * so legacy behaviour is to use those strings unmodified */
    dlg_editbox_set_utf8(ctrl, dp, text);
}

char *dlg_editbox_get_utf8(dlgcontrol *ctrl, dlgparam *dp)
{
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    assert(uc->ctrl->type == CTRL_EDITBOX);

#if GTK_CHECK_VERSION(2,4,0)
    if (uc->combo) {
        return dupstr(gtk_entry_get_text(
                          GTK_ENTRY(gtk_bin_get_child(GTK_BIN(uc->combo)))));
    }
#endif

    if (uc->entry) {
        return dupstr(gtk_entry_get_text(GTK_ENTRY(uc->entry)));
    }

    unreachable("bad control type in editbox_get");
}

char *dlg_editbox_get(dlgcontrol *ctrl, dlgparam *dp)
{
    /* GTK specifies that its edit boxes are always in UTF-8 anyway,
     * so legacy behaviour is to use those strings unmodified */
    return dlg_editbox_get_utf8(ctrl, dp);
}

void dlg_editbox_select_range(dlgcontrol *ctrl, dlgparam *dp,
                              size_t start, size_t len)
{
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    assert(uc->ctrl->type == CTRL_EDITBOX);

    GtkWidget *entry = NULL;

#if GTK_CHECK_VERSION(2,4,0)
    if (uc->combo)
        entry = gtk_bin_get_child(GTK_BIN(uc->combo));
#endif

    if (uc->entry)
        entry = uc->entry;

    assert(entry && "we should have a GtkEntry one way or another");

    gtk_editable_select_region(GTK_EDITABLE(entry), start, start + len);
}

#if !GTK_CHECK_VERSION(2,4,0)
static void container_remove_and_destroy(GtkWidget *w, gpointer data)
{
    GtkContainer *cont = GTK_CONTAINER(data);
    /* gtk_container_remove will unref the widget for us; we need not. */
    gtk_container_remove(cont, w);
}
#endif

/* The `listbox' functions can also apply to combo boxes. */
void dlg_listbox_clear(dlgcontrol *ctrl, dlgparam *dp)
{
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);

    assert(uc->ctrl->type == CTRL_EDITBOX ||
           uc->ctrl->type == CTRL_LISTBOX);

#if !GTK_CHECK_VERSION(2,4,0)
    if (uc->menu) {
        gtk_container_foreach(GTK_CONTAINER(uc->menu),
                              container_remove_and_destroy,
                              GTK_CONTAINER(uc->menu));
        return;
    }
    if (uc->list) {
        gtk_list_clear_items(GTK_LIST(uc->list), 0, -1);
        return;
    }
#endif
#if GTK_CHECK_VERSION(2,0,0)
    if (uc->listmodel) {
        gtk_list_store_clear(uc->listmodel);
        return;
    }
#endif
    unreachable("bad control type in listbox_clear");
}

void dlg_listbox_del(dlgcontrol *ctrl, dlgparam *dp, int index)
{
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);

    assert(uc->ctrl->type == CTRL_EDITBOX ||
           uc->ctrl->type == CTRL_LISTBOX);

#if !GTK_CHECK_VERSION(2,4,0)
    if (uc->menu) {
        gtk_container_remove(
            GTK_CONTAINER(uc->menu),
            g_list_nth_data(GTK_MENU_SHELL(uc->menu)->children, index));
        return;
    }
    if (uc->list) {
        gtk_list_clear_items(GTK_LIST(uc->list), index, index+1);
        return;
    }
#endif
#if GTK_CHECK_VERSION(2,0,0)
    if (uc->listmodel) {
        GtkTreePath *path;
        GtkTreeIter iter;
        assert(uc->listmodel != NULL);
        path = gtk_tree_path_new_from_indices(index, -1);
        gtk_tree_model_get_iter(GTK_TREE_MODEL(uc->listmodel), &iter, path);
        gtk_list_store_remove(uc->listmodel, &iter);
        gtk_tree_path_free(path);
        return;
    }
#endif
    unreachable("bad control type in listbox_del");
}

void dlg_listbox_add(dlgcontrol *ctrl, dlgparam *dp, char const *text)
{
    dlg_listbox_addwithid(ctrl, dp, text, 0);
}

/*
 * Each listbox entry may have a numeric id associated with it.
 * Note that some front ends only permit a string to be stored at
 * each position, which means that _if_ you put two identical
 * strings in any listbox then you MUST not assign them different
 * IDs and expect to get meaningful results back.
 */
void dlg_listbox_addwithid(dlgcontrol *ctrl, dlgparam *dp,
                           char const *text, int id)
{
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);

    assert(uc->ctrl->type == CTRL_EDITBOX ||
           uc->ctrl->type == CTRL_LISTBOX);

    /*
     * This routine is long and complicated in both GTK 1 and 2,
     * and completely different. Sigh.
     */
    dp->flags |= FLAG_UPDATING_COMBO_LIST;

#if !GTK_CHECK_VERSION(2,4,0)
    if (uc->menu) {
        /*
         * List item in a drop-down (but non-combo) list. Tabs are
         * ignored; we just provide a standard menu item with the
         * text.
         */
        GtkWidget *menuitem = gtk_menu_item_new_with_label(text);

        gtk_container_add(GTK_CONTAINER(uc->menu), menuitem);
        gtk_widget_show(menuitem);

        g_object_set_data(G_OBJECT(menuitem), "user-data",
                          GINT_TO_POINTER(id));
        g_signal_connect(G_OBJECT(menuitem), "activate",
                         G_CALLBACK(menuitem_activate), dp);
        goto done;
    }
    if (uc->list && uc->entry) {
        /*
         * List item in a combo-box list, which means the sensible
         * thing to do is make it a perfectly normal label. Hence
         * tabs are disregarded.
         */
        GtkWidget *listitem = gtk_list_item_new_with_label(text);

        gtk_container_add(GTK_CONTAINER(uc->list), listitem);
        gtk_widget_show(listitem);

        g_object_set_data(G_OBJECT(listitem), "user-data",
                          GINT_TO_POINTER(id));
        goto done;
    }
#endif
#if !GTK_CHECK_VERSION(2,0,0)
    if (uc->list) {
        /*
         * List item in a non-combo-box list box. We make all of
         * these Columns containing GtkLabels. This allows us to do
         * the nasty force_left hack irrespective of whether there
         * are tabs in the thing.
         */
        GtkWidget *listitem = gtk_list_item_new();
        GtkWidget *cols = columns_new(10);
        gint *percents;
        int i, ncols;

        /* Count the tabs in the text, and hence determine # of columns. */
        ncols = 1;
        for (i = 0; text[i]; i++)
            if (text[i] == '\t')
                ncols++;

        assert(ncols <=
               (uc->ctrl->listbox.ncols ? uc->ctrl->listbox.ncols : 1));
        percents = snewn(ncols, gint);
        percents[ncols-1] = 100;
        for (i = 0; i < ncols-1; i++) {
            percents[i] = uc->ctrl->listbox.percentages[i];
            percents[ncols-1] -= percents[i];
        }
        columns_set_cols(COLUMNS(cols), ncols, percents);
        sfree(percents);

        for (i = 0; i < ncols; i++) {
            int len = strcspn(text, "\t");
            char *dup = dupprintf("%.*s", len, text);
            GtkWidget *label;

            text += len;
            if (*text) text++;
            label = gtk_label_new(dup);
            sfree(dup);

            columns_add(COLUMNS(cols), label, i, 1);
            columns_force_left_align(COLUMNS(cols), label);
            gtk_widget_show(label);
        }
        gtk_container_add(GTK_CONTAINER(listitem), cols);
        gtk_widget_show(cols);
        gtk_container_add(GTK_CONTAINER(uc->list), listitem);
        gtk_widget_show(listitem);

        if (ctrl->listbox.multisel) {
            g_signal_connect(G_OBJECT(listitem), "key_press_event",
                             G_CALLBACK(listitem_multi_key), uc->adj);
        } else {
            g_signal_connect(G_OBJECT(listitem), "key_press_event",
                             G_CALLBACK(listitem_single_key), uc->adj);
        }
        g_signal_connect(G_OBJECT(listitem), "focus_in_event",
                         G_CALLBACK(widget_focus), dp);
        g_signal_connect(G_OBJECT(listitem), "button_press_event",
                         G_CALLBACK(listitem_button_press), dp);
        g_signal_connect(G_OBJECT(listitem), "button_release_event",
                         G_CALLBACK(listitem_button_release), dp);
        g_object_set_data(G_OBJECT(listitem), "user-data",
                          GINT_TO_POINTER(id));
        goto done;
    }
#else
    if (uc->listmodel) {
        GtkTreeIter iter;
        int i, cols;

        dp->flags |= FLAG_UPDATING_LISTBOX;/* inhibit drag-list update */
        gtk_list_store_append(uc->listmodel, &iter);
        dp->flags &= ~FLAG_UPDATING_LISTBOX;
        gtk_list_store_set(uc->listmodel, &iter, 0, id, -1);

        /*
         * Now go through text and divide it into columns at the tabs,
         * as necessary.
         */
        cols = (uc->ctrl->type == CTRL_LISTBOX ? ctrl->listbox.ncols : 1);
        cols = cols ? cols : 1;
        for (i = 0; i < cols; i++) {
            int collen = strcspn(text, "\t");
            char *tmpstr = mkstr(make_ptrlen(text, collen));
            gtk_list_store_set(uc->listmodel, &iter, i+1, tmpstr, -1);
            sfree(tmpstr);
            text += collen;
            if (*text) text++;
        }
        goto done;
    }
#endif
    unreachable("bad control type in listbox_addwithid");
  done:
    dp->flags &= ~FLAG_UPDATING_COMBO_LIST;
}

int dlg_listbox_getid(dlgcontrol *ctrl, dlgparam *dp, int index)
{
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);

    assert(uc->ctrl->type == CTRL_EDITBOX ||
           uc->ctrl->type == CTRL_LISTBOX);

#if !GTK_CHECK_VERSION(2,4,0)
    if (uc->menu || uc->list) {
        GList *children;
        GObject *item;

        children = gtk_container_children(GTK_CONTAINER(uc->menu ? uc->menu :
                                                        uc->list));
        item = G_OBJECT(g_list_nth_data(children, index));
        g_list_free(children);

        return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "user-data"));
    }
#endif
#if GTK_CHECK_VERSION(2,0,0)
    if (uc->listmodel) {
        GtkTreePath *path;
        GtkTreeIter iter;
        int ret;

        path = gtk_tree_path_new_from_indices(index, -1);
        gtk_tree_model_get_iter(GTK_TREE_MODEL(uc->listmodel), &iter, path);
        gtk_tree_model_get(GTK_TREE_MODEL(uc->listmodel), &iter, 0, &ret, -1);
        gtk_tree_path_free(path);

        return ret;
    }
#endif
    unreachable("bad control type in listbox_getid");
    return -1;                         /* placate dataflow analysis */
}

/* dlg_listbox_index returns <0 if no single element is selected. */
int dlg_listbox_index(dlgcontrol *ctrl, dlgparam *dp)
{
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);

    assert(uc->ctrl->type == CTRL_EDITBOX ||
           uc->ctrl->type == CTRL_LISTBOX);

#if !GTK_CHECK_VERSION(2,4,0)
    if (uc->menu || uc->list) {
        GList *children;
        GtkWidget *item, *activeitem;
        int i;
        int selected = -1;

        if (uc->menu)
            activeitem = gtk_menu_get_active(GTK_MENU(uc->menu));
        else
            activeitem = NULL;         /* unnecessarily placate gcc */

        children = gtk_container_children(GTK_CONTAINER(uc->menu ? uc->menu :
                                                        uc->list));
        for (i = 0; children!=NULL && (item = GTK_WIDGET(children->data))!=NULL;
             i++, children = children->next) {
            if (uc->menu ? activeitem == item :
                GTK_WIDGET_STATE(item) == GTK_STATE_SELECTED) {
                if (selected == -1)
                    selected = i;
                else
                    selected = -2;
            }
        }
        g_list_free(children);
        return selected < 0 ? -1 : selected;
    }
#else
    if (uc->combo) {
        /*
         * This API function already does the right thing in the
         * case of no current selection.
         */
        return gtk_combo_box_get_active(GTK_COMBO_BOX(uc->combo));
    }
#endif
#if GTK_CHECK_VERSION(2,0,0)
    if (uc->treeview) {
        GtkTreeSelection *treesel;
        GtkTreePath *path;
        GtkTreeModel *model;
        GList *sellist;
        gint *indices;
        int ret;

        assert(uc->treeview != NULL);
        treesel = gtk_tree_view_get_selection(GTK_TREE_VIEW(uc->treeview));

        if (gtk_tree_selection_count_selected_rows(treesel) != 1)
            return -1;

        sellist = gtk_tree_selection_get_selected_rows(treesel, &model);

        assert(sellist && sellist->data);
        path = sellist->data;

        if (gtk_tree_path_get_depth(path) != 1) {
            ret = -1;
        } else {
            indices = gtk_tree_path_get_indices(path);
            if (!indices) {
                ret = -1;
            } else {
                ret = indices[0];
            }
        }

        g_list_foreach(sellist, (GFunc)gtk_tree_path_free, NULL);
        g_list_free(sellist);

        return ret;
    }
#endif
    unreachable("bad control type in listbox_index");
    return -1;                         /* placate dataflow analysis */
}

bool dlg_listbox_issel(dlgcontrol *ctrl, dlgparam *dp, int index)
{
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);

    assert(uc->ctrl->type == CTRL_EDITBOX ||
           uc->ctrl->type == CTRL_LISTBOX);

#if !GTK_CHECK_VERSION(2,4,0)
    if (uc->menu || uc->list) {
        GList *children;
        GtkWidget *item, *activeitem;

        assert(uc->ctrl->type == CTRL_EDITBOX ||
               uc->ctrl->type == CTRL_LISTBOX);
        assert(uc->menu != NULL || uc->list != NULL);

        children = gtk_container_children(GTK_CONTAINER(uc->menu ? uc->menu :
                                                        uc->list));
        item = GTK_WIDGET(g_list_nth_data(children, index));
        g_list_free(children);

        if (uc->menu) {
            activeitem = gtk_menu_get_active(GTK_MENU(uc->menu));
            return item == activeitem;
        } else {
            return GTK_WIDGET_STATE(item) == GTK_STATE_SELECTED;
        }
    }
#else
    if (uc->combo) {
        /*
         * This API function already does the right thing in the
         * case of no current selection.
         */
        return gtk_combo_box_get_active(GTK_COMBO_BOX(uc->combo)) == index;
    }
#endif
#if GTK_CHECK_VERSION(2,0,0)
    if (uc->treeview) {
        GtkTreeSelection *treesel;
        GtkTreePath *path;
        bool ret;

        assert(uc->treeview != NULL);
        treesel = gtk_tree_view_get_selection(GTK_TREE_VIEW(uc->treeview));

        path = gtk_tree_path_new_from_indices(index, -1);
        ret = gtk_tree_selection_path_is_selected(treesel, path);
        gtk_tree_path_free(path);

        return ret;
    }
#endif
    unreachable("bad control type in listbox_issel");
    return false;                      /* placate dataflow analysis */
}

void dlg_listbox_select(dlgcontrol *ctrl, dlgparam *dp, int index)
{
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);

    assert(uc->ctrl->type == CTRL_EDITBOX ||
           uc->ctrl->type == CTRL_LISTBOX);

#if !GTK_CHECK_VERSION(2,4,0)
    if (uc->optmenu) {
        gtk_option_menu_set_history(GTK_OPTION_MENU(uc->optmenu), index);
        return;
    }
    if (uc->list) {
        int nitems;
        GList *items;
        gdouble newtop, newbot;

        gtk_list_select_item(GTK_LIST(uc->list), index);

        /*
         * Scroll the list box if necessary to ensure the newly
         * selected item is visible.
         */
        items = gtk_container_children(GTK_CONTAINER(uc->list));
        nitems = g_list_length(items);
        if (nitems > 0) {
            bool modified = false;
            g_list_free(items);
            newtop = uc->adj->lower +
                (uc->adj->upper - uc->adj->lower) * index / nitems;
            newbot = uc->adj->lower +
                (uc->adj->upper - uc->adj->lower) * (index+1) / nitems;
            if (uc->adj->value > newtop) {
                modified = true;
                uc->adj->value = newtop;
            } else if (uc->adj->value < newbot - uc->adj->page_size) {
                modified = true;
                uc->adj->value = newbot - uc->adj->page_size;
            }
            if (modified)
                gtk_adjustment_value_changed(uc->adj);
        }
        return;
    }
#else
    if (uc->combo) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(uc->combo), index);
        return;
    }
#endif
#if GTK_CHECK_VERSION(2,0,0)
    if (uc->treeview) {
        GtkTreeSelection *treesel;
        GtkTreePath *path;

        treesel = gtk_tree_view_get_selection(GTK_TREE_VIEW(uc->treeview));

        path = gtk_tree_path_new_from_indices(index, -1);
        gtk_tree_selection_select_path(treesel, path);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(uc->treeview),
                                     path, NULL, false, 0.0, 0.0);
        gtk_tree_path_free(path);
        return;
    }
#endif
    unreachable("bad control type in listbox_select");
}

void dlg_text_set(dlgcontrol *ctrl, dlgparam *dp, char const *text)
{
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);

    assert(uc->ctrl->type == CTRL_TEXT);
    assert(uc->text != NULL);

    gtk_label_set_text(GTK_LABEL(uc->text), text);
}

void dlg_label_change(dlgcontrol *ctrl, dlgparam *dp, char const *text)
{
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);

    switch (uc->ctrl->type) {
      case CTRL_BUTTON:
        gtk_label_set_text(GTK_LABEL(uc->toplevel), text);
        shortcut_highlight(uc->toplevel, ctrl->button.shortcut);
        break;
      case CTRL_CHECKBOX:
        gtk_label_set_text(GTK_LABEL(uc->toplevel), text);
        shortcut_highlight(uc->toplevel, ctrl->checkbox.shortcut);
        break;
      case CTRL_RADIO:
        gtk_label_set_text(GTK_LABEL(uc->label), text);
        shortcut_highlight(uc->label, ctrl->radio.shortcut);
        break;
      case CTRL_EDITBOX:
        gtk_label_set_text(GTK_LABEL(uc->label), text);
        shortcut_highlight(uc->label, ctrl->editbox.shortcut);
        break;
      case CTRL_FILESELECT:
        if (uc->label) {
            gtk_label_set_text(GTK_LABEL(uc->label), text);
            shortcut_highlight(uc->label, ctrl->fileselect.shortcut);
        }
        break;
      case CTRL_FONTSELECT:
        gtk_label_set_text(GTK_LABEL(uc->label), text);
        shortcut_highlight(uc->label, ctrl->fontselect.shortcut);
        break;
      case CTRL_LISTBOX:
        gtk_label_set_text(GTK_LABEL(uc->label), text);
        shortcut_highlight(uc->label, ctrl->listbox.shortcut);
        break;
      default:
        unreachable("bad control type in label_change");
    }
}

void dlg_filesel_set(dlgcontrol *ctrl, dlgparam *dp, Filename *fn)
{
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    /* We must copy fn->path before passing it to gtk_entry_set_text.
     * See comment in dlg_editbox_set() for the reasons. */
    char *duppath = dupstr(fn->path);
    assert(uc->ctrl->type == CTRL_FILESELECT);
    assert(uc->entry != NULL);
    gtk_entry_set_text(GTK_ENTRY(uc->entry), duppath);
    sfree(duppath);
}

Filename *dlg_filesel_get(dlgcontrol *ctrl, dlgparam *dp)
{
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    assert(uc->ctrl->type == CTRL_FILESELECT);
    if (!uc->entry) {
        assert(uc->textvalue);
        return filename_from_str(uc->textvalue);
    } else {
        return filename_from_str(gtk_entry_get_text(GTK_ENTRY(uc->entry)));
    }
}

void dlg_fontsel_set(dlgcontrol *ctrl, dlgparam *dp, FontSpec *fs)
{
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    /* We must copy fs->name before passing it to gtk_entry_set_text.
     * See comment in dlg_editbox_set() for the reasons. */
    char *dupname = dupstr(fs->name);
    assert(uc->ctrl->type == CTRL_FONTSELECT);
    assert(uc->entry != NULL);
    gtk_entry_set_text(GTK_ENTRY(uc->entry), dupname);
    sfree(dupname);
}

FontSpec *dlg_fontsel_get(dlgcontrol *ctrl, dlgparam *dp)
{
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    assert(uc->ctrl->type == CTRL_FONTSELECT);
    assert(uc->entry != NULL);
    return fontspec_new(gtk_entry_get_text(GTK_ENTRY(uc->entry)));
}

/*
 * Bracketing a large set of updates in these two functions will
 * cause the front end (if possible) to delay updating the screen
 * until it's all complete, thus avoiding flicker.
 */
void dlg_update_start(dlgcontrol *ctrl, dlgparam *dp)
{
    /*
     * Apparently we can't do this at all in GTK. GtkCList supports
     * freeze and thaw, but not GtkList. Bah.
     */
}

void dlg_update_done(dlgcontrol *ctrl, dlgparam *dp)
{
    /*
     * Apparently we can't do this at all in GTK. GtkCList supports
     * freeze and thaw, but not GtkList. Bah.
     */
}

void dlg_set_focus(dlgcontrol *ctrl, dlgparam *dp)
{
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);

    switch (ctrl->type) {
      case CTRL_CHECKBOX:
      case CTRL_BUTTON:
        /* Check boxes and buttons get the focus _and_ get toggled. */
        gtk_widget_grab_focus(uc->toplevel);
        break;
      case CTRL_FILESELECT:
      case CTRL_FONTSELECT:
      case CTRL_EDITBOX:
        if (uc->entry) {
            /* Anything containing an edit box gets that focused. */
            gtk_widget_grab_focus(uc->entry);
        }
#if GTK_CHECK_VERSION(2,4,0)
        else if (uc->combo) {
            /* Failing that, there'll be a combo box. */
            gtk_widget_grab_focus(uc->combo);
        }
#endif
        break;
      case CTRL_RADIO:
        /*
         * Radio buttons: we find the currently selected button and
         * focus it.
         */
        for (int i = 0; i < ctrl->radio.nbuttons; i++)
            if (gtk_toggle_button_get_active(
                    GTK_TOGGLE_BUTTON(uc->buttons[i]))) {
                gtk_widget_grab_focus(uc->buttons[i]);
            }
        break;
      case CTRL_LISTBOX:
#if !GTK_CHECK_VERSION(2,4,0)
        if (uc->optmenu) {
            gtk_widget_grab_focus(uc->optmenu);
            break;
        }
#else
        if (uc->combo) {
            gtk_widget_grab_focus(uc->combo);
            break;
        }
#endif
#if !GTK_CHECK_VERSION(2,0,0)
        if (uc->list) {
            /*
             * For GTK-1 style list boxes, we tell it to focus one
             * of its children, which appears to do the Right
             * Thing.
             */
            gtk_container_focus(GTK_CONTAINER(uc->list), GTK_DIR_TAB_FORWARD);
            break;
        }
#else
        if (uc->treeview) {
            gtk_widget_grab_focus(uc->treeview);
            break;
        }
#endif
        unreachable("bad control type in set_focus");
    }
}

/*
 * During event processing, you might well want to give an error
 * indication to the user. dlg_beep() is a quick and easy generic
 * error; dlg_error() puts up a message-box or equivalent.
 */
void dlg_beep(dlgparam *dp)
{
    gdk_display_beep(gdk_display_get_default());
}

static void set_transient_window_pos(GtkWidget *parent, GtkWidget *child)
{
#if !GTK_CHECK_VERSION(2,0,0)
    gint x, y, w, h, dx, dy;
    GtkRequisition req;
    gtk_window_set_position(GTK_WINDOW(child), GTK_WIN_POS_NONE);
    gtk_widget_size_request(GTK_WIDGET(child), &req);

    gdk_window_get_origin(gtk_widget_get_window(GTK_WIDGET(parent)), &x, &y);
    gdk_window_get_size(gtk_widget_get_window(GTK_WIDGET(parent)), &w, &h);

    /*
     * One corner of the transient will be offset inwards, by 1/4
     * of the parent window's size, from the corresponding corner
     * of the parent window. The corner will be chosen so as to
     * place the transient closer to the centre of the screen; this
     * should avoid transients going off the edge of the screen on
     * a regular basis.
     */
    if (x + w/2 < gdk_screen_width() / 2)
        dx = x + w/4;                  /* work from left edges */
    else
        dx = x + 3*w/4 - req.width;    /* work from right edges */
    if (y + h/2 < gdk_screen_height() / 2)
        dy = y + h/4;                  /* work from top edges */
    else
        dy = y + 3*h/4 - req.height;   /* work from bottom edges */
    gtk_widget_set_uposition(GTK_WIDGET(child), dx, dy);
#endif
}

void trivial_post_dialog_fn(void *vctx, int result)
{
}

void dlg_error_msg(dlgparam *dp, const char *msg)
{
    create_message_box(
        dp->window, "Error", msg,
        string_width("Some sort of text about a config-box error message"),
        false, &buttons_ok, trivial_post_dialog_fn, NULL);
}

/*
 * This function signals to the front end that the dialog's
 * processing is completed, and passes an integer value (typically
 * a success status).
 */
void dlg_end(dlgparam *dp, int value)
{
    dp->retval = value;
    gtk_widget_destroy(dp->window);
}

void dlg_refresh(dlgcontrol *ctrl, dlgparam *dp)
{
    struct uctrl *uc;

    if (ctrl) {
        if (ctrl->handler != NULL)
            ctrl->handler(ctrl, dp, dp->data, EVENT_REFRESH);
    } else {
        int i;

        for (i = 0; (uc = index234(dp->byctrl, i)) != NULL; i++) {
            assert(uc->ctrl != NULL);
            if (uc->ctrl->handler != NULL)
                uc->ctrl->handler(uc->ctrl, dp,
                                  dp->data, EVENT_REFRESH);
        }
    }
}

void dlg_coloursel_start(dlgcontrol *ctrl, dlgparam *dp, int r, int g, int b)
{
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);

#if GTK_CHECK_VERSION(3,0,0)
    GtkWidget *coloursel =
        gtk_color_chooser_dialog_new("Select a colour",
                                     GTK_WINDOW(dp->window));
    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(coloursel), false);
#else
    GtkWidget *okbutton, *cancelbutton;
    GtkWidget *coloursel =
        gtk_color_selection_dialog_new("Select a colour");
    GtkColorSelectionDialog *ccs = GTK_COLOR_SELECTION_DIALOG(coloursel);
    GtkColorSelection *cs = GTK_COLOR_SELECTION(
        gtk_color_selection_dialog_get_color_selection(ccs));
    gtk_color_selection_set_has_opacity_control(cs, false);
#endif

    dp->coloursel_result.ok = false;

    gtk_window_set_modal(GTK_WINDOW(coloursel), true);

#if GTK_CHECK_VERSION(3,0,0)
    {
        GdkRGBA rgba;
        rgba.red = r / 255.0;
        rgba.green = g / 255.0;
        rgba.blue = b / 255.0;
        rgba.alpha = 1.0;              /* fully opaque! */
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(coloursel), &rgba);
    }
#elif GTK_CHECK_VERSION(2,0,0)
    {
        GdkColor col;
        col.red = r * 0x0101;
        col.green = g * 0x0101;
        col.blue = b * 0x0101;
        gtk_color_selection_set_current_color(cs, &col);
    }
#else
    {
        gdouble cvals[4];
        cvals[0] = r / 255.0;
        cvals[1] = g / 255.0;
        cvals[2] = b / 255.0;
        cvals[3] = 1.0;                /* fully opaque! */
        gtk_color_selection_set_color(cs, cvals);
    }
#endif

    g_object_set_data(G_OBJECT(coloursel), "user-data", (gpointer)uc);

#if GTK_CHECK_VERSION(3,0,0)
    g_signal_connect(G_OBJECT(coloursel), "response",
                     G_CALLBACK(colourchoose_response), (gpointer)dp);
#else

#if GTK_CHECK_VERSION(2,0,0)
    g_object_get(G_OBJECT(ccs),
                 "ok-button", &okbutton,
                 "cancel-button", &cancelbutton,
                 (const char *)NULL);
#else
    okbutton = ccs->ok_button;
    cancelbutton = ccs->cancel_button;
#endif
    g_object_set_data(G_OBJECT(okbutton), "user-data",
                      (gpointer)coloursel);
    g_object_set_data(G_OBJECT(cancelbutton), "user-data",
                      (gpointer)coloursel);
    g_signal_connect(G_OBJECT(okbutton), "clicked",
                     G_CALLBACK(coloursel_ok), (gpointer)dp);
    g_signal_connect(G_OBJECT(cancelbutton), "clicked",
                     G_CALLBACK(coloursel_cancel), (gpointer)dp);
    g_signal_connect_swapped(G_OBJECT(okbutton), "clicked",
                             G_CALLBACK(gtk_widget_destroy),
                             (gpointer)coloursel);
    g_signal_connect_swapped(G_OBJECT(cancelbutton), "clicked",
                             G_CALLBACK(gtk_widget_destroy),
                             (gpointer)coloursel);
#endif
    gtk_widget_show(coloursel);
}

bool dlg_coloursel_results(dlgcontrol *ctrl, dlgparam *dp,
                           int *r, int *g, int *b)
{
    if (dp->coloursel_result.ok) {
        *r = dp->coloursel_result.r;
        *g = dp->coloursel_result.g;
        *b = dp->coloursel_result.b;
        return true;
    } else
        return false;
}

/* ----------------------------------------------------------------------
 * Signal handlers while the dialog box is active.
 */

static gboolean widget_focus(GtkWidget *widget, GdkEventFocus *event,
                             gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    struct uctrl *uc = dlg_find_bywidget(dp, widget);
    dlgcontrol *focus;

    if (uc && uc->ctrl)
        focus = uc->ctrl;
    else
        focus = NULL;

    if (focus != dp->currfocus) {
        dp->lastfocus = dp->currfocus;
        dp->currfocus = focus;
    }

    return false;
}

static void button_clicked(GtkButton *button, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    struct uctrl *uc = dlg_find_bywidget(dp, GTK_WIDGET(button));
    uc->ctrl->handler(uc->ctrl, dp, dp->data, EVENT_ACTION);
}

static void button_toggled(GtkToggleButton *tb, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    struct uctrl *uc = dlg_find_bywidget(dp, GTK_WIDGET(tb));
    uc->ctrl->handler(uc->ctrl, dp, dp->data, EVENT_VALCHANGE);
}

static gboolean editbox_key(GtkWidget *widget, GdkEventKey *event,
                            gpointer data)
{
    /*
     * GtkEntry has a nasty habit of eating the Return key, which
     * is unhelpful since it doesn't actually _do_ anything with it
     * (it calls gtk_widget_activate, but our edit boxes never need
     * activating). So I catch Return before GtkEntry sees it, and
     * pass it straight on to the parent widget. Effect: hitting
     * Return in an edit box will now activate the default button
     * in the dialog just like it will everywhere else.
     */
    GtkWidget *parent = gtk_widget_get_parent(widget);
    if (event->keyval == GDK_KEY_Return && parent != NULL) {
        gboolean return_val;
        g_signal_stop_emission_by_name(G_OBJECT(widget), "key_press_event");
        g_signal_emit_by_name(G_OBJECT(parent), "key_press_event",
                              event, &return_val);
        return return_val;
    }
    return false;
}

static void editbox_changed(GtkEditable *ed, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    if (!(dp->flags & FLAG_UPDATING_COMBO_LIST)) {
        struct uctrl *uc = dlg_find_bywidget(dp, GTK_WIDGET(ed));
        uc->ctrl->handler(uc->ctrl, dp, dp->data, EVENT_VALCHANGE);
    }
}

static gboolean editbox_lostfocus(GtkWidget *ed, GdkEventFocus *event,
                                  gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    struct uctrl *uc = dlg_find_bywidget(dp, GTK_WIDGET(ed));
    uc->ctrl->handler(uc->ctrl, dp, dp->data, EVENT_REFRESH);
    return false;
}

#if !GTK_CHECK_VERSION(2,0,0)

/*
 * GTK 1 list box event handlers.
 */

static gboolean listitem_key(GtkWidget *item, GdkEventKey *event,
                             gpointer data, bool multiple)
{
    GtkAdjustment *adj = GTK_ADJUSTMENT(data);

    if (event->keyval == GDK_Up || event->keyval == GDK_KP_Up ||
        event->keyval == GDK_Down || event->keyval == GDK_KP_Down ||
        event->keyval == GDK_Page_Up || event->keyval == GDK_KP_Page_Up ||
        event->keyval == GDK_Page_Down || event->keyval == GDK_KP_Page_Down) {
        /*
         * Up, Down, PgUp or PgDn have been pressed on a ListItem
         * in a list box. So, if the list box is single-selection:
         *
         *  - if the list item in question isn't already selected,
         *    we simply select it.
         *  - otherwise, we find the next one (or next
         *    however-far-away) in whichever direction we're going,
         *    and select that.
         *     + in this case, we must also fiddle with the
         *       scrollbar to ensure the newly selected item is
         *       actually visible.
         *
         * If it's multiple-selection, we do all of the above
         * except actually selecting anything, so we move the focus
         * and fiddle the scrollbar to follow it.
         */
        GtkWidget *list = item->parent;

        g_signal_stop_emission_by_name(G_OBJECT(item), "key_press_event");

        if (!multiple &&
            GTK_WIDGET_STATE(item) != GTK_STATE_SELECTED) {
            gtk_list_select_child(GTK_LIST(list), item);
        } else {
            int direction =
                (event->keyval==GDK_Up || event->keyval==GDK_KP_Up ||
                 event->keyval==GDK_Page_Up || event->keyval==GDK_KP_Page_Up)
                ? -1 : +1;
            int step =
                (event->keyval==GDK_Page_Down ||
                 event->keyval==GDK_KP_Page_Down ||
                 event->keyval==GDK_Page_Up || event->keyval==GDK_KP_Page_Up)
                ? 2 : 1;
            int i, n;
            GList *children, *chead;

            chead = children = gtk_container_children(GTK_CONTAINER(list));

            n = g_list_length(children);

            if (step == 2) {
                /*
                 * Figure out how many list items to a screenful,
                 * and adjust the step appropriately.
                 */
                step = 0.5 + adj->page_size * n / (adj->upper - adj->lower);
                step--;                /* go by one less than that */
            }

            i = 0;
            while (children != NULL) {
                if (item == children->data)
                    break;
                children = children->next;
                i++;
            }

            while (step > 0) {
                if (direction < 0 && i > 0)
                    children = children->prev, i--;
                else if (direction > 0 && i < n-1)
                    children = children->next, i++;
                step--;
            }

            if (children && children->data) {
                if (!multiple)
                    gtk_list_select_child(GTK_LIST(list),
                                          GTK_WIDGET(children->data));
                gtk_widget_grab_focus(GTK_WIDGET(children->data));
                gtk_adjustment_clamp_page(
                    adj,
                    adj->lower + (adj->upper-adj->lower) * i / n,
                    adj->lower + (adj->upper-adj->lower) * (i+1) / n);
            }

            g_list_free(chead);
        }
        return true;
    }

    return false;
}

static gboolean listitem_single_key(GtkWidget *item, GdkEventKey *event,
                                    gpointer data)
{
    return listitem_key(item, event, data, false);
}

static gboolean listitem_multi_key(GtkWidget *item, GdkEventKey *event,
                                   gpointer data)
{
    return listitem_key(item, event, data, true);
}

static gboolean listitem_button_press(GtkWidget *item, GdkEventButton *event,
                                      gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    struct uctrl *uc = dlg_find_bywidget(dp, GTK_WIDGET(item));
    switch (event->type) {
      default:
      case GDK_BUTTON_PRESS: uc->nclicks = 1; break;
      case GDK_2BUTTON_PRESS: uc->nclicks = 2; break;
      case GDK_3BUTTON_PRESS: uc->nclicks = 3; break;
    }
    return false;
}

static gboolean listitem_button_release(GtkWidget *item, GdkEventButton *event,
                                        gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    struct uctrl *uc = dlg_find_bywidget(dp, GTK_WIDGET(item));
    if (uc->nclicks>1) {
        uc->ctrl->handler(uc->ctrl, dp, dp->data, EVENT_ACTION);
        return true;
    }
    return false;
}

static void list_selchange(GtkList *list, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    struct uctrl *uc = dlg_find_bywidget(dp, GTK_WIDGET(list));
    if (!uc) return;
    uc->ctrl->handler(uc->ctrl, dp, dp->data, EVENT_SELCHANGE);
}

static void draglist_move(struct dlgparam *dp, struct uctrl *uc, int direction)
{
    int index = dlg_listbox_index(uc->ctrl, dp);
    GList *children = gtk_container_children(GTK_CONTAINER(uc->list));
    GtkWidget *child;

    if ((index < 0) ||
        (index == 0 && direction < 0) ||
        (index == g_list_length(children)-1 && direction > 0)) {
        gdk_display_beep(gdk_display_get_default());
        return;
    }

    child = g_list_nth_data(children, index);
    gtk_widget_ref(child);
    gtk_list_clear_items(GTK_LIST(uc->list), index, index+1);
    g_list_free(children);

    children = NULL;
    children = g_list_append(children, child);
    gtk_list_insert_items(GTK_LIST(uc->list), children, index + direction);
    gtk_list_select_item(GTK_LIST(uc->list), index + direction);
    uc->ctrl->handler(uc->ctrl, dp, dp->data, EVENT_VALCHANGE);
}

static void draglist_up(GtkButton *button, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    struct uctrl *uc = dlg_find_bywidget(dp, GTK_WIDGET(button));
    draglist_move(dp, uc, -1);
}

static void draglist_down(GtkButton *button, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    struct uctrl *uc = dlg_find_bywidget(dp, GTK_WIDGET(button));
    draglist_move(dp, uc, +1);
}

#else /* !GTK_CHECK_VERSION(2,0,0) */

/*
 * GTK 2 list box event handlers.
 */

static void listbox_doubleclick(GtkTreeView *treeview, GtkTreePath *path,
                                GtkTreeViewColumn *column, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    struct uctrl *uc = dlg_find_bywidget(dp, GTK_WIDGET(treeview));
    if (uc)
        uc->ctrl->handler(uc->ctrl, dp, dp->data, EVENT_ACTION);
}

static void listbox_selchange(GtkTreeSelection *treeselection,
                              gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    GtkTreeView *tree = gtk_tree_selection_get_tree_view(treeselection);
    struct uctrl *uc = dlg_find_bywidget(dp, GTK_WIDGET(tree));
    if (uc)
        uc->ctrl->handler(uc->ctrl, dp, dp->data, EVENT_SELCHANGE);
}

struct draglist_valchange_ctx {
    struct uctrl *uc;
    struct dlgparam *dp;
};

static gboolean draglist_valchange(gpointer data)
{
    struct draglist_valchange_ctx *ctx =
        (struct draglist_valchange_ctx *)data;

    ctx->uc->ctrl->handler(ctx->uc->ctrl, ctx->dp,
                           ctx->dp->data, EVENT_VALCHANGE);

    sfree(ctx);

    return false;
}

static void listbox_reorder(GtkTreeModel *treemodel, GtkTreePath *path,
                            GtkTreeIter *iter, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    gpointer tree;
    struct uctrl *uc;

    if (dp->flags & FLAG_UPDATING_LISTBOX)
        return;                        /* not a user drag operation */

    tree = g_object_get_data(G_OBJECT(treemodel), "user-data");
    uc = dlg_find_bywidget(dp, GTK_WIDGET(tree));
    if (uc) {
        /*
         * We should cause EVENT_VALCHANGE on the list box, now
         * that its rows have been reordered. However, the GTK 2
         * docs say that at the point this signal is received the
         * new row might not have actually been filled in yet.
         *
         * (So what smegging use is it then, eh? Don't suppose it
         * occurred to you at any point that letting the
         * application know _after_ the reordering was compelete
         * might be helpful to someone?)
         *
         * To get round this, I schedule an idle function, which I
         * hope won't be called until the main event loop is
         * re-entered after the drag-and-drop handler has finished
         * furtling with the list store.
         */
        struct draglist_valchange_ctx *ctx =
            snew(struct draglist_valchange_ctx);
        ctx->uc = uc;
        ctx->dp = dp;
        g_idle_add(draglist_valchange, ctx);
    }
}

#endif /* !GTK_CHECK_VERSION(2,0,0) */

#if !GTK_CHECK_VERSION(2,4,0)

static void menuitem_activate(GtkMenuItem *item, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    GtkWidget *menushell = GTK_WIDGET(item)->parent;
    gpointer optmenu = g_object_get_data(G_OBJECT(menushell), "user-data");
    struct uctrl *uc = dlg_find_bywidget(dp, GTK_WIDGET(optmenu));
    uc->ctrl->handler(uc->ctrl, dp, dp->data, EVENT_SELCHANGE);
}

#else

static void droplist_selchange(GtkComboBox *combo, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    struct uctrl *uc = dlg_find_bywidget(dp, GTK_WIDGET(combo));
    if (uc)
        uc->ctrl->handler(uc->ctrl, dp, dp->data, EVENT_SELCHANGE);
}

#endif /* !GTK_CHECK_VERSION(2,4,0) */

static void filechoose_emit_value(struct dlgparam *dp, struct uctrl *uc,
                                  const char *name)
{
    if (uc->entry) {
        gtk_entry_set_text(GTK_ENTRY(uc->entry), name);
    } else {
        uc->textvalue = name;
        uc->ctrl->handler(uc->ctrl, dp, dp->data, EVENT_ACTION);
        uc->textvalue = NULL;
    }
}

#ifdef USE_GTK_FILE_CHOOSER_DIALOG
static void filechoose_response(GtkDialog *dialog, gint response,
                                gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    struct uctrl *uc = g_object_get_data(G_OBJECT(dialog), "user-data");
    if (response == GTK_RESPONSE_ACCEPT) {
        gchar *name = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        filechoose_emit_value(dp, uc, name);
        g_free(name);
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}
#else
static void filesel_ok(GtkButton *button, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    gpointer filesel = g_object_get_data(G_OBJECT(button), "user-data");
    struct uctrl *uc = g_object_get_data(G_OBJECT(filesel), "user-data");
    const char *name = gtk_file_selection_get_filename(
        GTK_FILE_SELECTION(filesel));
    filechoose_emit_value(dp, uc, name);
}
#endif

static void fontsel_ok(GtkButton *button, gpointer data)
{
    /* struct dlgparam *dp = (struct dlgparam *)data; */

#if !GTK_CHECK_VERSION(2,0,0)

    gpointer fontsel = g_object_get_data(G_OBJECT(button), "user-data");
    struct uctrl *uc = g_object_get_data(G_OBJECT(fontsel), "user-data");
    const char *name = gtk_font_selection_dialog_get_font_name(
        GTK_FONT_SELECTION_DIALOG(fontsel));
    gtk_entry_set_text(GTK_ENTRY(uc->entry), name);

#else

    unifontsel *fontsel = (unifontsel *)g_object_get_data(
        G_OBJECT(button), "user-data");
    struct uctrl *uc = (struct uctrl *)fontsel->user_data;
    char *name = unifontsel_get_name(fontsel);
    assert(name);              /* should always be ok after OK pressed */
    gtk_entry_set_text(GTK_ENTRY(uc->entry), name);
    sfree(name);

#endif
}

#if GTK_CHECK_VERSION(3,0,0)

static void colourchoose_response(GtkDialog *dialog,
                                  gint response_id, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    struct uctrl *uc = g_object_get_data(G_OBJECT(dialog), "user-data");

    if (response_id == GTK_RESPONSE_OK) {
        GdkRGBA rgba;
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(dialog), &rgba);
        dp->coloursel_result.r = (int) (255 * rgba.red);
        dp->coloursel_result.g = (int) (255 * rgba.green);
        dp->coloursel_result.b = (int) (255 * rgba.blue);
        dp->coloursel_result.ok = true;
    } else {
        dp->coloursel_result.ok = false;
    }

    uc->ctrl->handler(uc->ctrl, dp, dp->data, EVENT_CALLBACK);

    gtk_widget_destroy(GTK_WIDGET(dialog));
}

#else /* GTK 1/2 coloursel response handlers */

static void coloursel_ok(GtkButton *button, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    gpointer coloursel = g_object_get_data(G_OBJECT(button), "user-data");
    struct uctrl *uc = g_object_get_data(G_OBJECT(coloursel), "user-data");

#if GTK_CHECK_VERSION(2,0,0)
    {
        GtkColorSelection *cs = GTK_COLOR_SELECTION(
            gtk_color_selection_dialog_get_color_selection(
                GTK_COLOR_SELECTION_DIALOG(coloursel)));
        GdkColor col;
        gtk_color_selection_get_current_color(cs, &col);
        dp->coloursel_result.r = col.red / 0x0100;
        dp->coloursel_result.g = col.green / 0x0100;
        dp->coloursel_result.b = col.blue / 0x0100;
    }
#else
    {
        GtkColorSelection *cs = GTK_COLOR_SELECTION(
            gtk_color_selection_dialog_get_color_selection(
                GTK_COLOR_SELECTION_DIALOG(coloursel)));
        gdouble cvals[4];
        gtk_color_selection_get_color(cs, cvals);
        dp->coloursel_result.r = (int) (255 * cvals[0]);
        dp->coloursel_result.g = (int) (255 * cvals[1]);
        dp->coloursel_result.b = (int) (255 * cvals[2]);
    }
#endif
    dp->coloursel_result.ok = true;
    uc->ctrl->handler(uc->ctrl, dp, dp->data, EVENT_CALLBACK);
}

static void coloursel_cancel(GtkButton *button, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    gpointer coloursel = g_object_get_data(G_OBJECT(button), "user-data");
    struct uctrl *uc = g_object_get_data(G_OBJECT(coloursel), "user-data");
    dp->coloursel_result.ok = false;
    uc->ctrl->handler(uc->ctrl, dp, dp->data, EVENT_CALLBACK);
}

#endif /* end of coloursel response handlers */

static void filefont_clicked(GtkButton *button, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    struct uctrl *uc = dlg_find_bywidget(dp, GTK_WIDGET(button));

    if (uc->ctrl->type == CTRL_FILESELECT) {
        /*
         * FIXME: do something about uc->ctrl->fileselect.filter
         */
#ifdef USE_GTK_FILE_CHOOSER_DIALOG
        GtkWidget *filechoose = gtk_file_chooser_dialog_new(
            uc->ctrl->fileselect.title, GTK_WINDOW(dp->window),
            (uc->ctrl->fileselect.for_writing ?
             GTK_FILE_CHOOSER_ACTION_SAVE :
             GTK_FILE_CHOOSER_ACTION_OPEN),
            STANDARD_CANCEL_LABEL, GTK_RESPONSE_CANCEL,
            STANDARD_OPEN_LABEL, GTK_RESPONSE_ACCEPT,
            (const gchar *)NULL);
        gtk_window_set_modal(GTK_WINDOW(filechoose), true);
        g_object_set_data(G_OBJECT(filechoose), "user-data", (gpointer)uc);
        g_signal_connect(G_OBJECT(filechoose), "response",
                         G_CALLBACK(filechoose_response), (gpointer)dp);
        gtk_widget_show(filechoose);
#else
        GtkWidget *filesel =
            gtk_file_selection_new(uc->ctrl->fileselect.title);
        gtk_window_set_modal(GTK_WINDOW(filesel), true);
        g_object_set_data(
            G_OBJECT(GTK_FILE_SELECTION(filesel)->ok_button), "user-data",
            (gpointer)filesel);
        g_object_set_data(G_OBJECT(filesel), "user-data", (gpointer)uc);
        g_signal_connect(
            G_OBJECT(GTK_FILE_SELECTION(filesel)->ok_button), "clicked",
            G_CALLBACK(filesel_ok), (gpointer)dp);
        g_signal_connect_swapped(
            G_OBJECT(GTK_FILE_SELECTION(filesel)->ok_button), "clicked",
            G_CALLBACK(gtk_widget_destroy), (gpointer)filesel);
        g_signal_connect_swapped(
            G_OBJECT(GTK_FILE_SELECTION(filesel)->cancel_button), "clicked",
            G_CALLBACK(gtk_widget_destroy), (gpointer)filesel);
        gtk_widget_show(filesel);
#endif
    }

    if (uc->ctrl->type == CTRL_FONTSELECT) {
        const gchar *fontname = gtk_entry_get_text(GTK_ENTRY(uc->entry));

#if !GTK_CHECK_VERSION(2,0,0)

        /*
         * Use the GTK 1 standard font selector.
         */

        gchar *spacings[] = { "c", "m", NULL };
        GtkWidget *fontsel =
            gtk_font_selection_dialog_new("Select a font");
        gtk_window_set_modal(GTK_WINDOW(fontsel), true);
        gtk_font_selection_dialog_set_filter(
            GTK_FONT_SELECTION_DIALOG(fontsel),
            GTK_FONT_FILTER_BASE, GTK_FONT_ALL,
            NULL, NULL, NULL, NULL, spacings, NULL);
        if (!gtk_font_selection_dialog_set_font_name(
                GTK_FONT_SELECTION_DIALOG(fontsel), fontname)) {
            /*
             * If the font name wasn't found as it was, try opening
             * it and extracting its FONT property. This should
             * have the effect of mapping short aliases into true
             * XLFDs.
             */
            GdkFont *font = gdk_font_load(fontname);
            if (font) {
                XFontStruct *xfs = GDK_FONT_XFONT(font);
                Display *disp = get_x11_display();
                Atom fontprop = XInternAtom(disp, "FONT", False);
                unsigned long ret;

                assert(disp); /* this is GTK1! */

                gdk_font_ref(font);
                if (XGetFontProperty(xfs, fontprop, &ret)) {
                    char *name = XGetAtomName(disp, (Atom)ret);
                    if (name)
                        gtk_font_selection_dialog_set_font_name(
                            GTK_FONT_SELECTION_DIALOG(fontsel), name);
                }
                gdk_font_unref(font);
            }
        }
        g_object_set_data(
            G_OBJECT(GTK_FONT_SELECTION_DIALOG(fontsel)->ok_button),
            "user-data", (gpointer)fontsel);
        g_object_set_data(G_OBJECT(fontsel), "user-data", (gpointer)uc);
        g_signal_connect(
            G_OBJECT(GTK_FONT_SELECTION_DIALOG(fontsel)->ok_button),
            "clicked", G_CALLBACK(fontsel_ok), (gpointer)dp);
        g_signal_connect_swapped(
            G_OBJECT(GTK_FONT_SELECTION_DIALOG(fontsel)->ok_button),
            "clicked", G_CALLBACK(gtk_widget_destroy),
            (gpointer)fontsel);
        g_signal_connect_swapped(
            G_OBJECT(GTK_FONT_SELECTION_DIALOG(fontsel)->cancel_button),
            "clicked", G_CALLBACK(gtk_widget_destroy),
            (gpointer)fontsel);
        gtk_widget_show(fontsel);

#else /* !GTK_CHECK_VERSION(2,0,0) */

        /*
         * Use the unifontsel code provided in unifont.c.
         */

        unifontsel *fontsel = unifontsel_new("Select a font");

        gtk_window_set_modal(fontsel->window, true);
        unifontsel_set_name(fontsel, fontname);

        g_object_set_data(G_OBJECT(fontsel->ok_button),
                          "user-data", (gpointer)fontsel);
        fontsel->user_data = uc;
        g_signal_connect(G_OBJECT(fontsel->ok_button), "clicked",
                         G_CALLBACK(fontsel_ok), (gpointer)dp);
        g_signal_connect_swapped(G_OBJECT(fontsel->ok_button), "clicked",
                                 G_CALLBACK(unifontsel_destroy),
                                 (gpointer)fontsel);
        g_signal_connect_swapped(G_OBJECT(fontsel->cancel_button),"clicked",
                                 G_CALLBACK(unifontsel_destroy),
                                 (gpointer)fontsel);

        gtk_widget_show(GTK_WIDGET(fontsel->window));

#endif /* !GTK_CHECK_VERSION(2,0,0) */

    }
}

#if !GTK_CHECK_VERSION(3,0,0)
static void label_sizealloc(GtkWidget *widget, GtkAllocation *alloc,
                            gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    struct uctrl *uc = dlg_find_bywidget(dp, widget);

    gtk_widget_set_size_request(uc->text, alloc->width, -1);
    gtk_label_set_text(GTK_LABEL(uc->text), uc->ctrl->label);
    g_signal_handler_disconnect(G_OBJECT(uc->text), uc->textsig);
}
#endif

/* ----------------------------------------------------------------------
 * This function does the main layout work: it reads a controlset,
 * it creates the relevant GTK controls, and returns a GtkWidget
 * containing the result. (This widget might be a title of some
 * sort, it might be a Columns containing many controls, or it
 * might be a GtkFrame containing a Columns; whatever it is, it's
 * definitely a GtkWidget and should probably be added to a
 * GtkVbox.)
 *
 * `win' is required for setting the default button. If it is
 * non-NULL, all buttons created will be default-capable (so they
 * have extra space round them for the default highlight).
 */
GtkWidget *layout_ctrls(
    struct dlgparam *dp, struct selparam *sp, struct Shortcuts *scs,
    struct controlset *s, GtkWindow *win)
{
    Columns *cols;
    GtkWidget *ret;
    int i;

    if (!s->boxname) {
        /* This controlset is a panel title. */
        assert(s->boxtitle);
        return gtk_label_new(s->boxtitle);
    }

    /*
     * Otherwise, we expect to be laying out actual controls, so
     * we'll start by creating a Columns for the purpose.
     */
    cols = COLUMNS(columns_new(4));
    ret = GTK_WIDGET(cols);
    gtk_widget_show(ret);

    /*
     * Create a containing frame if we have a box name.
     */
    if (*s->boxname) {
        ret = gtk_frame_new(s->boxtitle);   /* NULL is valid here */
        gtk_container_set_border_width(GTK_CONTAINER(cols), 4);
        gtk_container_add(GTK_CONTAINER(ret), GTK_WIDGET(cols));
        gtk_widget_show(ret);
    }

    /*
     * Now iterate through the controls themselves, create them,
     * and add them to the Columns.
     */
    for (i = 0; i < s->ncontrols; i++) {
        dlgcontrol *ctrl = s->ctrls[i];
        struct uctrl *uc;
        bool left = false;
        GtkWidget *w = NULL;

        switch (ctrl->type) {
          case CTRL_COLUMNS: {
            static const int simplecols[1] = { 100 };
            columns_set_cols(cols, ctrl->columns.ncols,
                             (ctrl->columns.percentages ?
                              ctrl->columns.percentages : simplecols));
            continue;                  /* no actual control created */
          }
          case CTRL_TABDELAY: {
            struct uctrl *uc = dlg_find_byctrl(dp, ctrl->tabdelay.ctrl);
            if (uc)
                columns_taborder_last(cols, uc->toplevel);
            continue;                  /* no actual control created */
          }
        }

        uc = snew(struct uctrl);
        uc->sp = sp;
        uc->ctrl = ctrl;
        uc->buttons = NULL;
        uc->entry = NULL;
#if !GTK_CHECK_VERSION(2,4,0)
        uc->list = uc->menu = uc->optmenu = NULL;
#else
        uc->combo = NULL;
#endif
#if GTK_CHECK_VERSION(2,0,0)
        uc->treeview = NULL;
        uc->listmodel = NULL;
#endif
        uc->button = uc->text = NULL;
        uc->label = NULL;
        uc->nclicks = 0;

        switch (ctrl->type) {
          case CTRL_BUTTON:
            w = gtk_button_new_with_label(ctrl->label);
            if (win) {
                gtk_widget_set_can_default(w, true);
                if (ctrl->button.isdefault)
                    gtk_window_set_default(win, w);
                if (ctrl->button.iscancel)
                    dp->cancelbutton = w;
            }
            g_signal_connect(G_OBJECT(w), "clicked",
                             G_CALLBACK(button_clicked), dp);
            g_signal_connect(G_OBJECT(w), "focus_in_event",
                             G_CALLBACK(widget_focus), dp);
            shortcut_add(scs, gtk_bin_get_child(GTK_BIN(w)),
                         ctrl->button.shortcut, SHORTCUT_UCTRL, uc);
            break;
          case CTRL_CHECKBOX:
            w = gtk_check_button_new_with_label(ctrl->label);
            g_signal_connect(G_OBJECT(w), "toggled",
                             G_CALLBACK(button_toggled), dp);
            g_signal_connect(G_OBJECT(w), "focus_in_event",
                             G_CALLBACK(widget_focus), dp);
            shortcut_add(scs, gtk_bin_get_child(GTK_BIN(w)),
                         ctrl->checkbox.shortcut, SHORTCUT_UCTRL, uc);
            left = true;
            break;
          case CTRL_RADIO: {
            /*
             * Radio buttons get to go inside their own Columns, no
             * matter what.
             */
            gint i, *percentages;
            GSList *group;

            w = columns_new(0);
            if (ctrl->label) {
                GtkWidget *label = gtk_label_new(ctrl->label);
                columns_add(COLUMNS(w), label, 0, 1);
                columns_force_left_align(COLUMNS(w), label);
                gtk_widget_show(label);
                shortcut_add(scs, label, ctrl->radio.shortcut,
                             SHORTCUT_UCTRL, uc);
                uc->label = label;
            }
            percentages = g_new(gint, ctrl->radio.ncolumns);
            for (i = 0; i < ctrl->radio.ncolumns; i++) {
                percentages[i] =
                    ((100 * (i+1) / ctrl->radio.ncolumns) -
                     100 * i / ctrl->radio.ncolumns);
            }
            columns_set_cols(COLUMNS(w), ctrl->radio.ncolumns,
                             percentages);
            g_free(percentages);
            group = NULL;

            uc->nbuttons = ctrl->radio.nbuttons;
            uc->buttons = snewn(uc->nbuttons, GtkWidget *);

            for (i = 0; i < ctrl->radio.nbuttons; i++) {
                GtkWidget *b;
                gint colstart;

                b = gtk_radio_button_new_with_label(
                    group, ctrl->radio.buttons[i]);
                uc->buttons[i] = b;
                group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(b));
                colstart = i % ctrl->radio.ncolumns;
                columns_add(COLUMNS(w), b, colstart,
                            (i == ctrl->radio.nbuttons-1 ?
                             ctrl->radio.ncolumns - colstart : 1));
                columns_force_left_align(COLUMNS(w), b);
                gtk_widget_show(b);
                g_signal_connect(G_OBJECT(b), "toggled",
                                 G_CALLBACK(button_toggled), dp);
                g_signal_connect(G_OBJECT(b), "focus_in_event",
                                 G_CALLBACK(widget_focus), dp);
                if (ctrl->radio.shortcuts) {
                    shortcut_add(scs, gtk_bin_get_child(GTK_BIN(b)),
                                 ctrl->radio.shortcuts[i],
                                 SHORTCUT_UCTRL, uc);
                }
            }
            break;
          }
          case CTRL_EDITBOX: {
            GtkWidget *signalobject;

            if (ctrl->editbox.has_list) {
#if !GTK_CHECK_VERSION(2,4,0)
                /*
                 * GTK 1 combo box.
                 */
                w = gtk_combo_new();
                gtk_combo_set_value_in_list(GTK_COMBO(w), false, true);
                uc->entry = GTK_COMBO(w)->entry;
                uc->list = GTK_COMBO(w)->list;
                signalobject = uc->entry;
#else
                /*
                 * GTK 2 combo box.
                 */
                uc->listmodel = gtk_list_store_new(2, G_TYPE_INT,
                                                   G_TYPE_STRING);
                w = gtk_combo_box_new_with_model_and_entry(
                    GTK_TREE_MODEL(uc->listmodel));
                g_object_set(G_OBJECT(w), "entry-text-column", 1,
                             (const char *)NULL);
                /* We cannot support password combo boxes. */
                assert(!ctrl->editbox.password);
                uc->combo = w;
                signalobject = uc->combo;
#endif
            } else {
                w = gtk_entry_new();
                if (ctrl->editbox.password)
                    gtk_entry_set_visibility(GTK_ENTRY(w), false);
                uc->entry = w;
                signalobject = w;
            }
            g_signal_connect(G_OBJECT(signalobject), "changed",
                             G_CALLBACK(editbox_changed), dp);
            g_signal_connect(G_OBJECT(signalobject), "key_press_event",
                             G_CALLBACK(editbox_key), dp);
            g_signal_connect(G_OBJECT(signalobject), "focus_in_event",
                             G_CALLBACK(widget_focus), dp);
            g_signal_connect(G_OBJECT(signalobject), "focus_out_event",
                             G_CALLBACK(editbox_lostfocus), dp);
            g_signal_connect(G_OBJECT(signalobject), "focus_out_event",
                             G_CALLBACK(editbox_lostfocus), dp);

#if !GTK_CHECK_VERSION(3,0,0)
            /*
             * Edit boxes, for some strange reason, have a minimum
             * width of 150 in GTK 1.2. We don't want this - we'd
             * rather the edit boxes acquired their natural width
             * from the column layout of the rest of the box.
             */
            {
                GtkRequisition req;
                gtk_widget_size_request(w, &req);
                gtk_widget_set_size_request(w, 10, req.height);
            }
#else
            /*
             * In GTK 3, this is still true, but there's a special
             * method for GtkEntry in particular to fix it.
             */
            if (GTK_IS_ENTRY(w))
                gtk_entry_set_width_chars(GTK_ENTRY(w), 1);
#endif

            if (ctrl->label) {
                GtkWidget *label;

                label = gtk_label_new(ctrl->label);

                shortcut_add(scs, label, ctrl->editbox.shortcut,
                             SHORTCUT_FOCUS, uc->entry);

                if (ctrl->editbox.percentwidth == 100) {
                    columns_add(cols, label,
                                COLUMN_START(ctrl->column),
                                COLUMN_SPAN(ctrl->column));
                    columns_force_left_align(cols, label);
                } else {
                    GtkWidget *container = columns_new(4);
                    gint percentages[2];
                    percentages[1] = ctrl->editbox.percentwidth;
                    percentages[0] = 100 - ctrl->editbox.percentwidth;
                    columns_set_cols(COLUMNS(container), 2, percentages);
                    columns_add(COLUMNS(container), label, 0, 1);
                    columns_force_left_align(COLUMNS(container), label);
                    columns_add(COLUMNS(container), w, 1, 1);
                    columns_align_next_to(COLUMNS(container), label, w);
                    gtk_widget_show(w);
                    w = container;
                }

                gtk_widget_show(label);
                uc->label = label;
            }
            break;
          }
          case CTRL_FILESELECT:
          case CTRL_FONTSELECT: {
            GtkWidget *ww;

            bool just_button = (ctrl->type == CTRL_FILESELECT &&
                                ctrl->fileselect.just_button);

            if (!just_button) {
                const char *browsebtn =
                    (ctrl->type == CTRL_FILESELECT ?
                     "Browse..." : "Change...");

                gint percentages[] = { 75, 25 };
                w = columns_new(4);
                columns_set_cols(COLUMNS(w), 2, percentages);

                if (ctrl->label) {
                    ww = gtk_label_new(ctrl->label);
                    columns_add(COLUMNS(w), ww, 0, 2);
                    columns_force_left_align(COLUMNS(w), ww);
                    gtk_widget_show(ww);
                    shortcut_add(scs, ww,
                                 (ctrl->type == CTRL_FILESELECT ?
                                  ctrl->fileselect.shortcut :
                                  ctrl->fontselect.shortcut),
                                 SHORTCUT_UCTRL, uc);
                    uc->label = ww;
                }

                uc->entry = ww = gtk_entry_new();
#if !GTK_CHECK_VERSION(3,0,0)
                {
                    GtkRequisition req;
                    gtk_widget_size_request(ww, &req);
                    gtk_widget_set_size_request(ww, 10, req.height);
                }
#else
                gtk_entry_set_width_chars(GTK_ENTRY(ww), 1);
#endif
                columns_add(COLUMNS(w), ww, 0, 1);
                gtk_widget_show(ww);

                uc->button = ww = gtk_button_new_with_label(browsebtn);
                columns_add(COLUMNS(w), ww, 1, 1);
                gtk_widget_show(ww);

                columns_align_next_to(COLUMNS(w), uc->entry, uc->button);

                g_signal_connect(G_OBJECT(uc->entry), "key_press_event",
                                 G_CALLBACK(editbox_key), dp);
                g_signal_connect(G_OBJECT(uc->entry), "changed",
                                 G_CALLBACK(editbox_changed), dp);
                g_signal_connect(G_OBJECT(uc->entry), "focus_in_event",
                                 G_CALLBACK(widget_focus), dp);
            } else {
                uc->button = w = gtk_button_new_with_label(ctrl->label);
                shortcut_add(scs, gtk_bin_get_child(GTK_BIN(w)),
                             ctrl->fileselect.shortcut, SHORTCUT_UCTRL, uc);
                gtk_widget_show(w);

            }
            g_signal_connect(G_OBJECT(uc->button), "focus_in_event",
                             G_CALLBACK(widget_focus), dp);
            g_signal_connect(G_OBJECT(uc->button), "clicked",
                             G_CALLBACK(filefont_clicked), dp);
            break;
          }
          case CTRL_LISTBOX:

#if GTK_CHECK_VERSION(2,0,0)
            /*
             * First construct the list data store, with the right
             * number of columns.
             */
#  if !GTK_CHECK_VERSION(2,4,0)
            /* (For GTK 2.0 to 2.3, we do this for full listboxes only,
             * because combo boxes are still done the old GTK1 way.) */
            if (ctrl->listbox.height > 0)
#  endif
            {
                GType *types;
                int i;
                int cols;

                cols = ctrl->listbox.ncols;
                cols = cols ? cols : 1;
                types = snewn(1 + cols, GType);

                types[0] = G_TYPE_INT;
                for (i = 0; i < cols; i++)
                    types[i+1] = G_TYPE_STRING;

                uc->listmodel = gtk_list_store_newv(1 + cols, types);

                sfree(types);
            }
#endif

            /*
             * See if it's a drop-down list (non-editable combo
             * box).
             */
            if (ctrl->listbox.height == 0) {
#if !GTK_CHECK_VERSION(2,4,0)
                /*
                 * GTK1 and early-GTK2 option-menu style of
                 * drop-down list.
                 */
                uc->optmenu = w = gtk_option_menu_new();
                uc->menu = gtk_menu_new();
                gtk_option_menu_set_menu(GTK_OPTION_MENU(w), uc->menu);
                g_object_set_data(G_OBJECT(uc->menu), "user-data",
                                  (gpointer)uc->optmenu);
                g_signal_connect(G_OBJECT(uc->optmenu), "focus_in_event",
                                 G_CALLBACK(widget_focus), dp);
#else
                /*
                 * Late-GTK2 style using a GtkComboBox.
                 */
                GtkCellRenderer *cr;

                /*
                 * Create a non-editable GtkComboBox (that is, not
                 * its subclass GtkComboBoxEntry).
                 */
                w = gtk_combo_box_new_with_model(
                    GTK_TREE_MODEL(uc->listmodel));
                uc->combo = w;

                /*
                 * Tell it how to render a list item (i.e. which
                 * column to look at in the list model).
                 */
                cr = gtk_cell_renderer_text_new();
                gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(w), cr, true);
                gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(w), cr,
                                               "text", 1, NULL);

                /*
                 * And tell it to notify us when the selection
                 * changes.
                 */
                g_signal_connect(G_OBJECT(w), "changed",
                                 G_CALLBACK(droplist_selchange), dp);

                g_signal_connect(G_OBJECT(w), "focus_in_event",
                                 G_CALLBACK(widget_focus), dp);
#endif
            } else {
#if !GTK_CHECK_VERSION(2,0,0)
                /*
                 * GTK1-style full list box.
                 */
                uc->list = gtk_list_new();
                if (ctrl->listbox.multisel == 2) {
                    gtk_list_set_selection_mode(GTK_LIST(uc->list),
                                                GTK_SELECTION_EXTENDED);
                } else if (ctrl->listbox.multisel == 1) {
                    gtk_list_set_selection_mode(GTK_LIST(uc->list),
                                                GTK_SELECTION_MULTIPLE);
                } else {
                    gtk_list_set_selection_mode(GTK_LIST(uc->list),
                                                GTK_SELECTION_SINGLE);
                }
                w = gtk_scrolled_window_new(NULL, NULL);
                gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(w),
                                                      uc->list);
                gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w),
                                               GTK_POLICY_NEVER,
                                               GTK_POLICY_AUTOMATIC);
                uc->adj = gtk_scrolled_window_get_vadjustment(
                    GTK_SCROLLED_WINDOW(w));

                gtk_widget_show(uc->list);
                g_signal_connect(G_OBJECT(uc->list), "selection-changed",
                                 G_CALLBACK(list_selchange), dp);
                g_signal_connect(G_OBJECT(uc->list), "focus_in_event",
                                 G_CALLBACK(widget_focus), dp);

                /*
                 * Adjust the height of the scrolled window to the
                 * minimum given by the height parameter.
                 *
                 * This piece of guesswork is a horrid hack based
                 * on looking inside the GTK 1.2 sources
                 * (specifically gtkviewport.c, which appears to be
                 * the widget which provides the border around the
                 * scrolling area). Anyone lets me know how I can
                 * do this in a way which isn't at risk from GTK
                 * upgrades, I'd be grateful.
                 */
                {
                    int edge;
                    edge = GTK_WIDGET(uc->list)->style->klass->ythickness;
                    gtk_widget_set_size_request(
                        w, 10, 2*edge + (ctrl->listbox.height *
                                         get_listitemheight(w)));
                }

                if (ctrl->listbox.draglist) {
                    /*
                     * GTK doesn't appear to make it easy to
                     * implement a proper draggable list; so
                     * instead I'm just going to have to put an Up
                     * and a Down button to the right of the actual
                     * list box. Ah well.
                     */
                    GtkWidget *cols, *button;
                    static const gint percentages[2] = { 80, 20 };

                    cols = columns_new(4);
                    columns_set_cols(COLUMNS(cols), 2, percentages);
                    columns_add(COLUMNS(cols), w, 0, 1);
                    gtk_widget_show(w);
                    button = gtk_button_new_with_label("Up");
                    columns_add(COLUMNS(cols), button, 1, 1);
                    gtk_widget_show(button);
                    g_signal_connect(G_OBJECT(button), "clicked",
                                     G_CALLBACK(draglist_up), dp);
                    g_signal_connect(G_OBJECT(button), "focus_in_event",
                                     G_CALLBACK(widget_focus), dp);
                    button = gtk_button_new_with_label("Down");
                    columns_add(COLUMNS(cols), button, 1, 1);
                    gtk_widget_show(button);
                    g_signal_connect(G_OBJECT(button), "clicked",
                                     G_CALLBACK(draglist_down), dp);
                    g_signal_connect(G_OBJECT(button), "focus_in_event",
                                     G_CALLBACK(widget_focus), dp);

                    w = cols;
                }
#else
                /*
                 * GTK2 treeview-based full list box.
                 */
                GtkTreeSelection *sel;

                /*
                 * Create the list box itself, its columns, and
                 * its containing scrolled window.
                 */
                w = gtk_tree_view_new_with_model(
                    GTK_TREE_MODEL(uc->listmodel));
                g_object_set_data(G_OBJECT(uc->listmodel), "user-data",
                                  (gpointer)w);
                gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(w), false);
                sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(w));
                gtk_tree_selection_set_mode(
                    sel, ctrl->listbox.multisel ? GTK_SELECTION_MULTIPLE :
                    GTK_SELECTION_SINGLE);
                uc->treeview = w;
                g_signal_connect(G_OBJECT(w), "row-activated",
                                 G_CALLBACK(listbox_doubleclick), dp);
                g_signal_connect(G_OBJECT(w), "focus_in_event",
                                 G_CALLBACK(widget_focus), dp);
                g_signal_connect(G_OBJECT(sel), "changed",
                                 G_CALLBACK(listbox_selchange), dp);

                if (ctrl->listbox.draglist) {
                    gtk_tree_view_set_reorderable(GTK_TREE_VIEW(w), true);
                    g_signal_connect(G_OBJECT(uc->listmodel), "row-inserted",
                                     G_CALLBACK(listbox_reorder), dp);
                }

                {
                    int i;
                    int cols;

                    cols = ctrl->listbox.ncols;
                    cols = cols ? cols : 1;
                    for (i = 0; i < cols; i++) {
                        GtkTreeViewColumn *column;
                        GtkCellRenderer *cellrend;
                        /*
                         * It appears that GTK 2 doesn't leave us any
                         * particularly sensible way to honour the
                         * "percentages" specification in the ctrl
                         * structure.
                         */
                        cellrend = gtk_cell_renderer_text_new();
                        if (!ctrl->listbox.hscroll) {
                            g_object_set(G_OBJECT(cellrend),
                                         "ellipsize", PANGO_ELLIPSIZE_END,
                                         "ellipsize-set", true,
                                         (const char *)NULL);
                        }
                        column = gtk_tree_view_column_new_with_attributes(
                            "heading", cellrend, "text", i+1, (char *)NULL);
                        gtk_tree_view_column_set_sizing(
                            column, GTK_TREE_VIEW_COLUMN_GROW_ONLY);
                        gtk_tree_view_append_column(GTK_TREE_VIEW(w), column);
                    }
                }

                {
                    GtkWidget *scroll;

                    scroll = gtk_scrolled_window_new(NULL, NULL);
                    gtk_scrolled_window_set_shadow_type(
                        GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
                    gtk_widget_show(w);
                    gtk_container_add(GTK_CONTAINER(scroll), w);
                    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                                   GTK_POLICY_AUTOMATIC,
                                                   GTK_POLICY_ALWAYS);
                    gtk_widget_set_size_request(
                        scroll, -1,
                        ctrl->listbox.height * get_listitemheight(w));

                    w = scroll;
                }
#endif
            }

            if (ctrl->label) {
                GtkWidget *label, *container;

                label = gtk_label_new(ctrl->label);
#if GTK_CHECK_VERSION(3,0,0)
                gtk_label_set_width_chars(GTK_LABEL(label), 3);
#endif

                shortcut_add(scs, label, ctrl->listbox.shortcut,
                             SHORTCUT_UCTRL, uc);

                container = columns_new(4);
                if (ctrl->listbox.percentwidth == 100) {
                    columns_add(COLUMNS(container), label, 0, 1);
                    columns_force_left_align(COLUMNS(container), label);
                    columns_add(COLUMNS(container), w, 0, 1);
                } else {
                    gint percentages[2];
                    percentages[1] = ctrl->listbox.percentwidth;
                    percentages[0] = 100 - ctrl->listbox.percentwidth;
                    columns_set_cols(COLUMNS(container), 2, percentages);
                    columns_add(COLUMNS(container), label, 0, 1);
                    columns_force_left_align(COLUMNS(container), label);
                    columns_add(COLUMNS(container), w, 1, 1);
                    columns_align_next_to(COLUMNS(container), label, w);
                }
                gtk_widget_show(label);
                gtk_widget_show(w);

                w = container;
                uc->label = label;
            }

            break;
          case CTRL_TEXT:
#if !GTK_CHECK_VERSION(3,0,0)
            /*
             * Wrapping text widgets don't sit well with the GTK2
             * layout model, in which widgets state a minimum size
             * and the whole window then adjusts to the smallest
             * size it can sensibly take given its contents. A
             * wrapping text widget _has_ no clear minimum size;
             * instead it has a range of possibilities. It can be
             * one line deep but 2000 wide, or two lines deep and
             * 1000 pixels, or three by 867, or four by 500 and so
             * on. It can be as short as you like provided you
             * don't mind it being wide, or as narrow as you like
             * provided you don't mind it being tall.
             *
             * Therefore, it fits very badly into the layout model.
             * Hence the only thing to do is pick a width and let
             * it choose its own number of lines. To do this I'm
             * going to cheat a little. All new wrapping text
             * widgets will be created with a minimal text content
             * "X"; then, after the rest of the dialog box is set
             * up and its size calculated, the text widgets will be
             * told their width and given their real text, which
             * will cause the size to be recomputed in the y
             * direction (because many of them will expand to more
             * than one line).
             */
            uc->text = w = gtk_label_new("X");
            uc->textsig =
                g_signal_connect(G_OBJECT(w), "size-allocate",
                                 G_CALLBACK(label_sizealloc), dp);
#else
            /*
             * In GTK3, this is all fixed, because the main aim of the
             * new 'height-for-width' geometry management is to make
             * wrapping labels behave sensibly. So now we can just do
             * the obvious thing.
             */
            uc->text = w = gtk_label_new(uc->ctrl->label);
#endif
#if GTK_CHECK_VERSION(2,0,0)
            gtk_label_set_selectable(GTK_LABEL(w), true);
            gtk_widget_set_can_focus(w, false);
#endif
            align_label_left(GTK_LABEL(w));
            gtk_label_set_line_wrap(GTK_LABEL(w), ctrl->text.wrap);
            if (!ctrl->text.wrap) {
                gtk_widget_show(uc->text);
                w = gtk_scrolled_window_new(NULL, NULL);
                gtk_container_set_border_width(GTK_CONTAINER(w), 0);
                gtk_container_add(GTK_CONTAINER(w), uc->text);
                gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w),
                                               GTK_POLICY_AUTOMATIC,
                                               GTK_POLICY_NEVER);
#if GTK_CHECK_VERSION(2,0,0)
                gtk_widget_set_can_focus(w, false);
#endif
            }
            break;
        }

        assert(w != NULL);

        columns_add(cols, w,
                    COLUMN_START(ctrl->column),
                    COLUMN_SPAN(ctrl->column));
        if (left)
            columns_force_left_align(cols, w);
        if (ctrl->align_next_to) {
            struct uctrl *uc2 = dlg_find_byctrl(
                dp, ctrl->align_next_to);
            assert(uc2);
            columns_align_next_to(cols, w, uc2->toplevel);

#if GTK_CHECK_VERSION(3, 10, 0)
            /* Slightly nicer to align baselines than just vertically
             * centring, where the option is available */
            gtk_widget_set_valign(w, GTK_ALIGN_BASELINE);
            gtk_widget_set_valign(uc2->toplevel, GTK_ALIGN_BASELINE);
#endif
        }
        gtk_widget_show(w);

        uc->toplevel = w;
        dlg_add_uctrl(dp, uc);
    }

    return ret;
}

struct selparam {
    struct dlgparam *dp;
    GtkNotebook *panels;
    GtkWidget *panel;
#if !GTK_CHECK_VERSION(2,0,0)
    GtkWidget *treeitem;
#else
    int depth;
    GtkTreePath *treepath;
#endif
    struct Shortcuts shortcuts;
};

#if GTK_CHECK_VERSION(2,0,0)
static void treeselection_changed(GtkTreeSelection *treeselection,
                                  gpointer data)
{
    struct selparam **sps = (struct selparam **)data, *sp;
    GtkTreeModel *treemodel;
    GtkTreeIter treeiter;
    gint spindex;
    gint page_num;

    if (!gtk_tree_selection_get_selected(treeselection, &treemodel, &treeiter))
        return;

    gtk_tree_model_get(treemodel, &treeiter, TREESTORE_PARAMS, &spindex, -1);
    sp = sps[spindex];

    page_num = gtk_notebook_page_num(sp->panels, sp->panel);
    gtk_notebook_set_current_page(sp->panels, page_num);

    sp->dp->curr_panel = sp;
    dlg_refresh(NULL, sp->dp);

    sp->dp->shortcuts = &sp->shortcuts;
}
#else
static void treeitem_sel(GtkItem *item, gpointer data)
{
    struct selparam *sp = (struct selparam *)data;
    gint page_num;

    page_num = gtk_notebook_page_num(sp->panels, sp->panel);
    gtk_notebook_set_page(sp->panels, page_num);

    sp->dp->curr_panel = sp;
    dlg_refresh(NULL, sp->dp);

    sp->dp->shortcuts = &sp->shortcuts;
    sp->dp->currtreeitem = sp->treeitem;
}
#endif

bool dlg_is_visible(dlgcontrol *ctrl, dlgparam *dp)
{
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    /*
     * A control is visible if it belongs to _no_ notebook page (i.e.
     * it's one of the config-box-global buttons like Load or About),
     * or if it belongs to the currently selected page.
     */
    return uc->sp == NULL || uc->sp == dp->curr_panel;
}

#if !GTK_CHECK_VERSION(2,0,0)
static bool tree_grab_focus(struct dlgparam *dp)
{
    int i, f;

    /*
     * See if any of the treeitems has the focus.
     */
    f = -1;
    for (i = 0; i < dp->ntreeitems; i++)
        if (GTK_WIDGET_HAS_FOCUS(dp->treeitems[i])) {
            f = i;
            break;
        }

    if (f >= 0)
        return false;
    else {
        gtk_widget_grab_focus(dp->currtreeitem);
        return true;
    }
}

gint tree_focus(GtkContainer *container, GtkDirectionType direction,
                gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;

    g_signal_stop_emission_by_name(G_OBJECT(container), "focus");
    /*
     * If there's a focused treeitem, we return false to cause the
     * focus to move on to some totally other control. If not, we
     * focus the selected one.
     */
    return tree_grab_focus(dp);
}
#endif

gint win_key_press(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;

    if (event->keyval == GDK_KEY_Escape && dp->cancelbutton) {
        g_signal_emit_by_name(G_OBJECT(dp->cancelbutton), "clicked");
        return true;
    }

    if ((event->state & GDK_MOD1_MASK) &&
        (unsigned char)event->string[0] > 0 &&
        (unsigned char)event->string[0] <= 127) {
        int schr = (unsigned char)event->string[0];
        struct Shortcut *sc = &dp->shortcuts->sc[schr];

        switch (sc->action) {
          case SHORTCUT_TREE:
#if GTK_CHECK_VERSION(2,0,0)
            gtk_widget_grab_focus(sc->widget);
#else
            tree_grab_focus(dp);
#endif
            break;
          case SHORTCUT_FOCUS:
            gtk_widget_grab_focus(sc->widget);
            break;
          case SHORTCUT_UCTRL:
            /*
             * We must do something sensible with a uctrl.
             * Precisely what this is depends on the type of
             * control.
             */
            switch (sc->uc->ctrl->type) {
              case CTRL_CHECKBOX:
              case CTRL_BUTTON:
                /* Check boxes and buttons get the focus _and_ get toggled. */
                gtk_widget_grab_focus(sc->uc->toplevel);
                g_signal_emit_by_name(G_OBJECT(sc->uc->toplevel), "clicked");
                break;
              case CTRL_FILESELECT:
              case CTRL_FONTSELECT:
                /* File/font selectors have their buttons pressed (ooer),
                 * and focus transferred to the edit box. */
                g_signal_emit_by_name(G_OBJECT(sc->uc->button), "clicked");
                if (sc->uc->entry)
                    gtk_widget_grab_focus(sc->uc->entry);
                break;
              case CTRL_RADIO:
                /*
                 * Radio buttons are fun, because they have
                 * multiple shortcuts. We must find whether the
                 * activated shortcut is the shortcut for the whole
                 * group, or for a particular button. In the former
                 * case, we find the currently selected button and
                 * focus it; in the latter, we focus-and-click the
                 * button whose shortcut was pressed.
                 */
                if (schr == sc->uc->ctrl->radio.shortcut) {
                    int i;
                    for (i = 0; i < sc->uc->ctrl->radio.nbuttons; i++)
                        if (gtk_toggle_button_get_active(
                                GTK_TOGGLE_BUTTON(sc->uc->buttons[i]))) {
                            gtk_widget_grab_focus(sc->uc->buttons[i]);
                        }
                } else if (sc->uc->ctrl->radio.shortcuts) {
                    int i;
                    for (i = 0; i < sc->uc->ctrl->radio.nbuttons; i++)
                        if (schr == sc->uc->ctrl->radio.shortcuts[i]) {
                            gtk_widget_grab_focus(sc->uc->buttons[i]);
                            g_signal_emit_by_name(
                                G_OBJECT(sc->uc->buttons[i]), "clicked");
                        }
                }
                break;
              case CTRL_LISTBOX:

#if !GTK_CHECK_VERSION(2,4,0)
                if (sc->uc->optmenu) {
                    GdkEventButton bev;
                    gint returnval;

                    gtk_widget_grab_focus(sc->uc->optmenu);
                    /* Option menus don't work using the "clicked" signal.
                     * We need to manufacture a button press event :-/ */
                    bev.type = GDK_BUTTON_PRESS;
                    bev.button = 1;
                    g_signal_emit_by_name(G_OBJECT(sc->uc->optmenu),
                                          "button_press_event",
                                          &bev, &returnval);
                    break;
                }
#else
                if (sc->uc->combo) {
                    gtk_widget_grab_focus(sc->uc->combo);
                    gtk_combo_box_popup(GTK_COMBO_BOX(sc->uc->combo));
                    break;
                }
#endif
#if !GTK_CHECK_VERSION(2,0,0)
                if (sc->uc->list) {
                    /*
                     * For GTK-1 style list boxes, we tell it to
                     * focus one of its children, which appears to
                     * do the Right Thing.
                     */
                    gtk_container_focus(GTK_CONTAINER(sc->uc->list),
                                        GTK_DIR_TAB_FORWARD);
                    break;
                }
#else
                if (sc->uc->treeview) {
                    gtk_widget_grab_focus(sc->uc->treeview);
                    break;
                }
#endif
                unreachable("bad listbox type in win_key_press");
            }
            break;
        }
    }

    return false;
}

#if !GTK_CHECK_VERSION(2,0,0)
gint tree_key_press(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;

    if (event->keyval == GDK_Up || event->keyval == GDK_KP_Up ||
        event->keyval == GDK_Down || event->keyval == GDK_KP_Down) {
        int dir, i, j = -1;
        for (i = 0; i < dp->ntreeitems; i++)
            if (widget == dp->treeitems[i])
                break;
        if (i < dp->ntreeitems) {
            if (event->keyval == GDK_Up || event->keyval == GDK_KP_Up)
                dir = -1;
            else
                dir = +1;

            while (1) {
                i += dir;
                if (i < 0 || i >= dp->ntreeitems)
                    break;             /* nothing in that dir to select */
                /*
                 * Determine if this tree item is visible.
                 */
                {
                    GtkWidget *w = dp->treeitems[i];
                    bool vis = true;
                    while (w && (GTK_IS_TREE_ITEM(w) || GTK_IS_TREE(w))) {
                        if (!GTK_WIDGET_VISIBLE(w)) {
                            vis = false;
                            break;
                        }
                        w = w->parent;
                    }
                    if (vis) {
                        j = i;         /* got one */
                        break;
                    }
                }
            }
        }
        g_signal_stop_emission_by_name(G_OBJECT(widget), "key_press_event");
        if (j >= 0) {
            g_signal_emit_by_name(G_OBJECT(dp->treeitems[j]), "toggle");
            gtk_widget_grab_focus(dp->treeitems[j]);
        }
        return true;
    }

    /*
     * It's nice for Left and Right to expand and collapse tree
     * branches.
     */
    if (event->keyval == GDK_Left || event->keyval == GDK_KP_Left) {
        g_signal_stop_emission_by_name(G_OBJECT(widget), "key_press_event");
        gtk_tree_item_collapse(GTK_TREE_ITEM(widget));
        return true;
    }
    if (event->keyval == GDK_Right || event->keyval == GDK_KP_Right) {
        g_signal_stop_emission_by_name(G_OBJECT(widget), "key_press_event");
        gtk_tree_item_expand(GTK_TREE_ITEM(widget));
        return true;
    }

    return false;
}
#endif

static void shortcut_highlight(GtkWidget *labelw, int chr)
{
    GtkLabel *label = GTK_LABEL(labelw);
    const gchar *currstr;
    gchar *pattern;
    int i;

#if !GTK_CHECK_VERSION(2,0,0)
    {
        gchar *currstr_nonconst;
        gtk_label_get(label, &currstr_nonconst);
        currstr = currstr_nonconst;
    }
#else
    currstr = gtk_label_get_text(label);
#endif

    for (i = 0; currstr[i]; i++)
        if (tolower((unsigned char)currstr[i]) == chr) {
            pattern = dupprintf("%*s_", i, "");
            gtk_label_set_pattern(label, pattern);
            sfree(pattern);
            break;
        }
}

void shortcut_add(struct Shortcuts *scs, GtkWidget *labelw,
                  int chr, int action, void *ptr)
{
    if (chr == NO_SHORTCUT)
        return;

    chr = tolower((unsigned char)chr);

    assert(scs->sc[chr].action == SHORTCUT_EMPTY);

    scs->sc[chr].action = action;

    if (action == SHORTCUT_FOCUS || action == SHORTCUT_TREE) {
        scs->sc[chr].uc = NULL;
        scs->sc[chr].widget = (GtkWidget *)ptr;
    } else {
        scs->sc[chr].widget = NULL;
        scs->sc[chr].uc = (struct uctrl *)ptr;
    }

    shortcut_highlight(labelw, chr);
}

static int get_listitemheight(GtkWidget *w)
{
#if !GTK_CHECK_VERSION(2,0,0)
    GtkWidget *listitem = gtk_list_item_new_with_label("foo");
    GtkRequisition req;
    gtk_widget_size_request(listitem, &req);
    g_object_ref_sink(G_OBJECT(listitem));
    return req.height;
#else
    int height;
    GtkCellRenderer *cr = gtk_cell_renderer_text_new();
#if GTK_CHECK_VERSION(3,0,0)
    {
        GtkRequisition req;
        /*
         * Since none of my list items wraps in this GUI, no
         * interesting width-for-height behaviour should be happening,
         * so I don't think it should matter here whether I ask for
         * the minimum or natural height.
         */
        gtk_cell_renderer_get_preferred_size(cr, w, &req, NULL);
        height = req.height;
    }
#else
    gtk_cell_renderer_get_size(cr, w, NULL, NULL, NULL, NULL, &height);
#endif
    g_object_ref(G_OBJECT(cr));
    g_object_ref_sink(G_OBJECT(cr));
    g_object_unref(G_OBJECT(cr));
    return height;
#endif
}

#if GTK_CHECK_VERSION(2,0,0)
void initial_treeview_collapse(struct dlgparam *dp, GtkWidget *tree)
{
    /*
     * Collapse the deeper branches of the treeview into the state we
     * like them to start off in. See comment below in do_config_box.
     */
    int i;
    for (i = 0; i < dp->nselparams; i++)
        if (dp->selparams[i]->depth >= 2)
            gtk_tree_view_collapse_row(GTK_TREE_VIEW(tree),
                                       dp->selparams[i]->treepath);
}
#endif

#if GTK_CHECK_VERSION(3,0,0)
void treeview_map_event(GtkWidget *tree, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(tree, &alloc);
    gtk_widget_set_size_request(tree, alloc.width, -1);
    initial_treeview_collapse(dp, tree);
}
#endif

GtkWidget *create_config_box(const char *title, Conf *conf,
                             bool midsession, int protcfginfo,
                             post_dialog_fn_t after, void *afterctx)
{
    GtkWidget *window, *hbox, *vbox, *cols, *label,
        *tree, *treescroll, *panels, *panelvbox;
    int index, level, protocol;
    char *path;
#if GTK_CHECK_VERSION(2,0,0)
    GtkTreeStore *treestore;
    GtkCellRenderer *treerenderer;
    GtkTreeViewColumn *treecolumn;
    GtkTreeSelection *treeselection;
    GtkTreeIter treeiterlevels[8];
#else
    GtkTreeItem *treeitemlevels[8];
    GtkTree *treelevels[8];
#endif
    struct dlgparam *dp;
    struct Shortcuts scs;

    struct selparam **selparams = NULL;
    size_t nselparams = 0, selparamsize = 0;

    dp = snew(struct dlgparam);
    dp->after = after;
    dp->afterctx = afterctx;

    dlg_init(dp);

    for (index = 0; index < lenof(scs.sc); index++) {
        scs.sc[index].action = SHORTCUT_EMPTY;
    }

    window = our_dialog_new();

    dp->ctrlbox = ctrl_new_box();
    protocol = conf_get_int(conf, CONF_protocol);
    setup_config_box(dp->ctrlbox, midsession, protocol, protcfginfo);
    unix_setup_config_box(dp->ctrlbox, midsession, protocol);
    gtk_setup_config_box(dp->ctrlbox, midsession, window);

    gtk_window_set_title(GTK_WINDOW(window), title);
    hbox = gtk_hbox_new(false, 4);
    our_dialog_add_to_content_area(GTK_WINDOW(window), hbox, true, true, 0);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), 10);
    gtk_widget_show(hbox);
    vbox = gtk_vbox_new(false, 4);
    gtk_box_pack_start(GTK_BOX(hbox), vbox, false, false, 0);
    gtk_widget_show(vbox);
    cols = columns_new(4);
    gtk_box_pack_start(GTK_BOX(vbox), cols, false, false, 0);
    gtk_widget_show(cols);
    label = gtk_label_new("Category:");
    columns_add(COLUMNS(cols), label, 0, 1);
    columns_force_left_align(COLUMNS(cols), label);
    gtk_widget_show(label);
    treescroll = gtk_scrolled_window_new(NULL, NULL);
#if GTK_CHECK_VERSION(2,0,0)
    treestore = gtk_tree_store_new(
        TREESTORE_NUM, G_TYPE_STRING, G_TYPE_INT);
    tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(treestore));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), false);
    treerenderer = gtk_cell_renderer_text_new();
    treecolumn = gtk_tree_view_column_new_with_attributes(
        "Label", treerenderer, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), treecolumn);
    treeselection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
    gtk_tree_selection_set_mode(treeselection, GTK_SELECTION_BROWSE);
    gtk_container_add(GTK_CONTAINER(treescroll), tree);
#else
    tree = gtk_tree_new();
    gtk_tree_set_view_mode(GTK_TREE(tree), GTK_TREE_VIEW_ITEM);
    gtk_tree_set_selection_mode(GTK_TREE(tree), GTK_SELECTION_BROWSE);
    g_signal_connect(G_OBJECT(tree), "focus", G_CALLBACK(tree_focus), dp);
#endif
    g_signal_connect(G_OBJECT(tree), "focus_in_event",
                     G_CALLBACK(widget_focus), dp);
    shortcut_add(&scs, label, 'g', SHORTCUT_TREE, tree);
    gtk_widget_show(treescroll);
    gtk_box_pack_start(GTK_BOX(vbox), treescroll, true, true, 0);
    panels = gtk_notebook_new();
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(panels), false);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(panels), false);
    gtk_box_pack_start(GTK_BOX(hbox), panels, true, true, 0);
    gtk_widget_show(panels);

    panelvbox = NULL;
    path = NULL;
    level = 0;
    for (index = 0; index < dp->ctrlbox->nctrlsets; index++) {
        struct controlset *s = dp->ctrlbox->ctrlsets[index];
        GtkWidget *w;

        if (!*s->pathname) {
            w = layout_ctrls(dp, NULL, &scs, s, GTK_WINDOW(window));

            our_dialog_set_action_area(GTK_WINDOW(window), w);
        } else {
            int j = path ? ctrl_path_compare(s->pathname, path) : 0;
            if (j != INT_MAX) {        /* add to treeview, start new panel */
                char *c;
#if GTK_CHECK_VERSION(2,0,0)
                GtkTreeIter treeiter;
#else
                GtkWidget *treeitem;
#endif
                bool first;

                /*
                 * We expect never to find an implicit path
                 * component. For example, we expect never to see
                 * A/B/C followed by A/D/E, because that would
                 * _implicitly_ create A/D. All our path prefixes
                 * are expected to contain actual controls and be
                 * selectable in the treeview; so we would expect
                 * to see A/D _explicitly_ before encountering
                 * A/D/E.
                 */
                assert(j == ctrl_path_elements(s->pathname) - 1);

                c = strrchr(s->pathname, '/');
                if (!c)
                    c = s->pathname;
                else
                    c++;

                path = s->pathname;

                first = (panelvbox == NULL);

                panelvbox = gtk_vbox_new(false, 4);
                gtk_widget_show(panelvbox);
                gtk_notebook_append_page(GTK_NOTEBOOK(panels), panelvbox,
                                         NULL);

                struct selparam *sp = snew(struct selparam);

                if (first) {
                    gint page_num;

                    page_num = gtk_notebook_page_num(GTK_NOTEBOOK(panels),
                                                     panelvbox);
                    gtk_notebook_set_current_page(GTK_NOTEBOOK(panels),
                                                  page_num);

                    dp->curr_panel = sp;
                }

                sgrowarray(selparams, selparamsize, nselparams);
                selparams[nselparams] = sp;
                sp->dp = dp;
                sp->panels = GTK_NOTEBOOK(panels);
                sp->panel = panelvbox;
                sp->shortcuts = scs;   /* structure copy */

                assert(j-1 < level);

#if GTK_CHECK_VERSION(2,0,0)
                if (j > 0)
                    /* treeiterlevels[j-1] will always be valid because we
                     * don't allow implicit path components; see above.
                     */
                    gtk_tree_store_append(treestore, &treeiter,
                                          &treeiterlevels[j-1]);
                else
                    gtk_tree_store_append(treestore, &treeiter, NULL);
                gtk_tree_store_set(treestore, &treeiter,
                                   TREESTORE_PATH, c,
                                   TREESTORE_PARAMS, nselparams,
                                   -1);
                treeiterlevels[j] = treeiter;

                sp->depth = j;
                if (j > 0) {
                    sp->treepath = gtk_tree_model_get_path(
                        GTK_TREE_MODEL(treestore), &treeiterlevels[j-1]);
                    /*
                     * We are going to collapse all tree branches
                     * at depth greater than 2, but not _yet_; see
                     * the comment at the call to
                     * gtk_tree_view_collapse_row below.
                     */
                    gtk_tree_view_expand_row(GTK_TREE_VIEW(tree),
                                             sp->treepath, false);
                } else {
                    sp->treepath = NULL;
                }
#else
                treeitem = gtk_tree_item_new_with_label(c);
                if (j > 0) {
                    if (!treelevels[j-1]) {
                        treelevels[j-1] = GTK_TREE(gtk_tree_new());
                        gtk_tree_item_set_subtree(
                            treeitemlevels[j-1], GTK_WIDGET(treelevels[j-1]));
                        if (j < 2)
                            gtk_tree_item_expand(treeitemlevels[j-1]);
                        else
                            gtk_tree_item_collapse(treeitemlevels[j-1]);
                    }
                    gtk_tree_append(treelevels[j-1], treeitem);
                } else {
                    gtk_tree_append(GTK_TREE(tree), treeitem);
                }
                treeitemlevels[j] = GTK_TREE_ITEM(treeitem);
                treelevels[j] = NULL;

                g_signal_connect(G_OBJECT(treeitem), "key_press_event",
                                 G_CALLBACK(tree_key_press), dp);
                g_signal_connect(G_OBJECT(treeitem), "focus_in_event",
                                 G_CALLBACK(widget_focus), dp);

                gtk_widget_show(treeitem);

                if (first)
                    gtk_tree_select_child(GTK_TREE(tree), treeitem);
                sp->treeitem = treeitem;
#endif

                level = j+1;
                nselparams++;
            }

            w = layout_ctrls(dp, selparams[nselparams-1],
                             &selparams[nselparams-1]->shortcuts, s, NULL);
            gtk_box_pack_start(GTK_BOX(panelvbox), w, false, false, 0);
            gtk_widget_show(w);
        }
    }

#if GTK_CHECK_VERSION(2,0,0)
    /*
     * We want our tree view to come up with all branches at depth 2
     * or more collapsed. However, if we start off with those branches
     * collapsed, then the tree view's size request will be calculated
     * based on the width of the collapsed tree, and then when the
     * collapsed branches are expanded later, the tree view will
     * jarringly change size.
     *
     * So instead we start with everything expanded; then, once the
     * tree view has computed its resulting width requirement, we
     * collapse the relevant rows, but force the width to be the value
     * we just retrieved. This arranges that the tree view is wide
     * enough to have all branches expanded without further resizing.
     */

    dp->nselparams = nselparams;
    dp->selparams = selparams;

#if !GTK_CHECK_VERSION(3,0,0)
    {
        /*
         * In GTK2, we can just do the job right now.
         */
        GtkRequisition req;
        gtk_widget_size_request(tree, &req);
        initial_treeview_collapse(dp, tree);
        gtk_widget_set_size_request(tree, req.width, -1);
    }
#else
    /*
     * But in GTK3, we have to wait until the widget is about to be
     * mapped, because the size computation won't have been done yet.
     */
    g_signal_connect(G_OBJECT(tree), "map",
                     G_CALLBACK(treeview_map_event), dp);
#endif /* GTK 2 vs 3 */
#endif /* GTK 2+ vs 1 */

#if GTK_CHECK_VERSION(2,0,0)
    g_signal_connect(G_OBJECT(treeselection), "changed",
                     G_CALLBACK(treeselection_changed), selparams);
#else
    dp->ntreeitems = nselparams;
    dp->treeitems = snewn(dp->ntreeitems, GtkWidget *);
    for (index = 0; index < nselparams; index++) {
        g_signal_connect(G_OBJECT(selparams[index]->treeitem), "select",
                         G_CALLBACK(treeitem_sel),
                         selparams[index]);
        dp->treeitems[index] = selparams[index]->treeitem;
    }
#endif

    dp->data = conf;
    dlg_refresh(NULL, dp);

    dp->shortcuts = &selparams[0]->shortcuts;
#if !GTK_CHECK_VERSION(2,0,0)
    dp->currtreeitem = dp->treeitems[0];
#endif
    dp->lastfocus = NULL;
    dp->retval = -1;
    dp->window = window;

    set_window_icon(window, cfg_icon, n_cfg_icon);

#if !GTK_CHECK_VERSION(2,0,0)
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(treescroll),
                                          tree);
#endif
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(treescroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_show(tree);

    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    gtk_widget_show(window);

    /*
     * Set focus into the first available control.
     */
    for (index = 0; index < dp->ctrlbox->nctrlsets; index++) {
        struct controlset *s = dp->ctrlbox->ctrlsets[index];
        bool done = false;
        int j;

        if (*s->pathname) {
            for (j = 0; j < s->ncontrols; j++)
                if (s->ctrls[j]->type != CTRL_TABDELAY &&
                    s->ctrls[j]->type != CTRL_COLUMNS &&
                    s->ctrls[j]->type != CTRL_TEXT) {
                    dlg_set_focus(s->ctrls[j], dp);
                    dp->lastfocus = s->ctrls[j];
                    done = true;
                    break;
                }
        }
        if (done)
            break;
    }

    g_signal_connect(G_OBJECT(window), "destroy",
                     G_CALLBACK(dlgparam_destroy), dp);
    g_signal_connect(G_OBJECT(window), "key_press_event",
                     G_CALLBACK(win_key_press), dp);

    return window;
}

static void dlgparam_destroy(GtkWidget *widget, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    dp->after(dp->afterctx, dp->retval);
    dlg_cleanup(dp);
    ctrl_free_box(dp->ctrlbox);
#if GTK_CHECK_VERSION(2,0,0)
    if (dp->selparams) {
        for (size_t i = 0; i < dp->nselparams; i++) {
            if (dp->selparams[i]->treepath)
                gtk_tree_path_free(dp->selparams[i]->treepath);
            sfree(dp->selparams[i]);
        }
        sfree(dp->selparams);
        dp->selparams = NULL;
    }
#endif
    /*
     * Instead of freeing dp right now, defer it until we return to
     * the GTK main loop. Then if any other last-minute GTK events
     * happen while the rest of the widgets are being cleaned up, our
     * handlers will still be able to try to look things up in dp.
     * (They won't find anything - we've just emptied it - but at
     * least they won't crash while trying.)
     */
    queue_toplevel_callback(sfree, dp);
}

static void messagebox_handler(dlgcontrol *ctrl, dlgparam *dp,
                               void *data, int event)
{
    if (event == EVENT_ACTION)
        dlg_end(dp, ctrl->context.i);
}

static const struct message_box_button button_array_yn[] = {
    {"Yes", 'y', +1, 1},
    {"No", 'n', -1, 0},
};
const struct message_box_buttons buttons_yn = {
    button_array_yn, lenof(button_array_yn),
};
static const struct message_box_button button_array_ok[] = {
    {"OK", 'o', 1, 1},
};
const struct message_box_buttons buttons_ok = {
    button_array_ok, lenof(button_array_ok),
};

static GtkWidget *create_message_box_general(
    GtkWidget *parentwin, const char *title, const char *msg, int minwid,
    bool selectable, const struct message_box_buttons *buttons,
    post_dialog_fn_t after, void *afterctx,
    GtkWidget *(*action_postproc)(GtkWidget *, void *), void *postproc_ctx)
{
    GtkWidget *window, *w0, *w1;
    struct controlset *s0, *s1;
    dlgcontrol *c, *textctrl;
    struct dlgparam *dp;
    struct Shortcuts scs;
    int i, index, ncols, min_type;

    dp = snew(struct dlgparam);
    dp->after = after;
    dp->afterctx = afterctx;

    dlg_init(dp);

    for (index = 0; index < lenof(scs.sc); index++) {
        scs.sc[index].action = SHORTCUT_EMPTY;
    }

    dp->ctrlbox = ctrl_new_box();

    /*
     * Count up the number of buttons and find out what kinds there
     * are.
     */
    ncols = 0;
    min_type = +1;
    for (i = 0; i < buttons->nbuttons; i++) {
        const struct message_box_button *button = &buttons->buttons[i];
        ncols++;
        if (min_type > button->type)
            min_type = button->type;
        assert(button->value >= 0);    /* <0 means no return value available */
    }

    s0 = ctrl_getset(dp->ctrlbox, "", "", "");
    c = ctrl_columns(s0, 2, 50, 50);
    c->columns.ncols = s0->ncolumns = ncols;
    c->columns.percentages = sresize(c->columns.percentages, ncols, int);
    for (index = 0; index < ncols; index++)
        c->columns.percentages[index] = (index+1)*100/ncols - index*100/ncols;
    index = 0;
    for (i = 0; i < buttons->nbuttons; i++) {
        const struct message_box_button *button = &buttons->buttons[i];
        c = ctrl_pushbutton(s0, button->title, button->shortcut,
                            HELPCTX(no_help), messagebox_handler,
                            I(button->value));
        c->column = index++;
        if (button->type > 0)
            c->button.isdefault = true;

        /* We always arrange that _some_ button is labelled as
         * 'iscancel', so that pressing Escape will always cause
         * win_key_press to do something. The button we choose is
         * whichever has the smallest type value: this means that real
         * cancel buttons (labelled -1) will be picked if one is
         * there, or in cases where the options are yes/no (1,0) then
         * no will be picked, and if there's only one option (a box
         * that really is just showing a _message_ and not even asking
         * a question) then that will be picked. */
        if (button->type == min_type)
            c->button.iscancel = true;
    }

    s1 = ctrl_getset(dp->ctrlbox, "x", "", "");
    textctrl = ctrl_text(s1, msg, HELPCTX(no_help));

    window = our_dialog_new();
    gtk_window_set_title(GTK_WINDOW(window), title);
    w0 = layout_ctrls(dp, NULL, &scs, s0, GTK_WINDOW(window));
    if (action_postproc)
        w0 = action_postproc(w0, postproc_ctx);
    our_dialog_set_action_area(GTK_WINDOW(window), w0);
    gtk_widget_show(w0);
    w1 = layout_ctrls(dp, NULL, &scs, s1, GTK_WINDOW(window));
    gtk_container_set_border_width(GTK_CONTAINER(w1), 10);
    gtk_widget_set_size_request(w1, minwid+20, -1);
    our_dialog_add_to_content_area(GTK_WINDOW(window), w1, true, true, 0);
    gtk_widget_show(w1);

    dp->shortcuts = &scs;
    dp->lastfocus = NULL;
    dp->retval = 0;
    dp->window = window;

    if (parentwin) {
        set_transient_window_pos(parentwin, window);
        gtk_window_set_transient_for(GTK_WINDOW(window),
                                     GTK_WINDOW(parentwin));
    } else
        gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    gtk_container_set_focus_child(GTK_CONTAINER(window), NULL);
    gtk_widget_show(window);
    gtk_window_set_focus(GTK_WINDOW(window), NULL);

#if GTK_CHECK_VERSION(2,0,0)
    if (selectable) {
        /*
         * GTK selectable labels have a habit of selecting their
         * entire contents when they gain focus. As far as I can see,
         * an individual GtkLabel has no way to turn this off - source
         * diving suggests that the only configurable option for it is
         * "gtk-label-select-on-focus" in the cross-application
         * GtkSettings, and there's no per-label or even
         * per-application override.
         *
         * It's ugly to have text in a message box start up all
         * selected, and also it interferes with any PRIMARY selection
         * you might already have had. So for this purpose we'd prefer
         * that the text doesn't _start off_ selected, but it should
         * be selectable later.
         *
         * So we make the label selectable _now_, after the widget is
         * shown and the focus has already gone wherever it's going.
         */
        struct uctrl *uc = dlg_find_byctrl(dp, textctrl);
        gtk_label_select_region(GTK_LABEL(uc->text), 0, 0);
        gtk_label_set_selectable(GTK_LABEL(uc->text), true);
    }
#else
    (void)textctrl;                    /* placate warning */
#endif

#if !GTK_CHECK_VERSION(2,0,0)
    dp->currtreeitem = NULL;
    dp->treeitems = NULL;
#else
    dp->selparams = NULL;
#endif

    g_signal_connect(G_OBJECT(window), "destroy",
                     G_CALLBACK(dlgparam_destroy), dp);
    g_signal_connect(G_OBJECT(window), "key_press_event",
                     G_CALLBACK(win_key_press), dp);

    return window;
}

GtkWidget *create_message_box(
    GtkWidget *parentwin, const char *title, const char *msg, int minwid,
    bool selectable, const struct message_box_buttons *buttons,
    post_dialog_fn_t after, void *afterctx)
{
    return create_message_box_general(
        parentwin, title, msg, minwid, selectable, buttons, after, afterctx,
        NULL /* action_postproc */, NULL /* postproc_ctx */);
}

struct confirm_ssh_host_key_dialog_ctx {
    char *host;
    int port;
    char *keytype;
    char *keystr;
    char *more_info;
    void (*callback)(void *callback_ctx, SeatPromptResult result);
    void *callback_ctx;
    Seat *seat;

    GtkWidget *main_dialog;
    GtkWidget *more_info_dialog;
};

static void confirm_ssh_host_key_result_callback(void *vctx, int result)
{
    struct confirm_ssh_host_key_dialog_ctx *ctx =
        (struct confirm_ssh_host_key_dialog_ctx *)vctx;

    if (result >= 0) {
        SeatPromptResult logical_result;

        /*
         * Convert the dialog-box return value (one of three
         * possibilities) into the return value we pass back to the SSH
         * code (one of only two possibilities, because the SSH code
         * doesn't care whether we saved the host key or not).
         */
        if (result == 2) {
            store_host_key(ctx->seat, ctx->host, ctx->port,
                           ctx->keytype, ctx->keystr);
            logical_result = SPR_OK;
        } else if (result == 1) {
            logical_result = SPR_OK;
        } else {
            logical_result = SPR_USER_ABORT;
        }

        ctx->callback(ctx->callback_ctx, logical_result);
    }

    /*
     * Clean up this context structure, whether or not a result was
     * ever actually delivered from the dialog box.
     */
    unregister_dialog(ctx->seat, DIALOG_SLOT_NETWORK_PROMPT);

    if (ctx->more_info_dialog)
        gtk_widget_destroy(ctx->more_info_dialog);

    sfree(ctx->host);
    sfree(ctx->keytype);
    sfree(ctx->keystr);
    sfree(ctx->more_info);
    sfree(ctx);
}

static GtkWidget *add_more_info_button(GtkWidget *w, void *vctx)
{
    GtkWidget *box = gtk_hbox_new(false, 10);
    gtk_widget_show(box);
    gtk_box_pack_end(GTK_BOX(box), w, false, true, 0);
    GtkWidget *button = gtk_button_new_with_label("More info...");
    gtk_widget_show(button);
    gtk_box_pack_start(GTK_BOX(box), button, false, true, 0);
    *(GtkWidget **)vctx = button;
    return box;
}

static void more_info_closed(void *vctx, int result)
{
    struct confirm_ssh_host_key_dialog_ctx *ctx =
        (struct confirm_ssh_host_key_dialog_ctx *)vctx;

    ctx->more_info_dialog = NULL;
}

static void more_info_button_clicked(GtkButton *button, gpointer vctx)
{
    struct confirm_ssh_host_key_dialog_ctx *ctx =
        (struct confirm_ssh_host_key_dialog_ctx *)vctx;

    if (ctx->more_info_dialog)
        return;

    ctx->more_info_dialog = create_message_box(
        ctx->main_dialog, "Host key information", ctx->more_info,
        string_width("SHA256 fingerprint: ecdsa-sha2-nistp521 521 "
                     "abcdefghkmnopqrsuvwxyzABCDEFGHJKLMNOPQRSTUW"), true,
        &buttons_ok, more_info_closed, ctx);
}

const SeatDialogPromptDescriptions *gtk_seat_prompt_descriptions(Seat *seat)
{
    static const SeatDialogPromptDescriptions descs = {
        .hk_accept_action = "press \"Accept\"",
        .hk_connect_once_action = "press \"Connect Once\"",
        .hk_cancel_action = "press \"Cancel\"",
        .hk_cancel_action_Participle = "Pressing \"Cancel\"",
        .weak_accept_action = "press \"Yes\"",
        .weak_cancel_action = "press \"No\"",
    };
    return &descs;
}

/*
 * Format a SeatDialogText into a strbuf, also adjusting the box width
 * to cope with displayed text. Returns the dialog box title.
 */
static const char *gtk_format_seatdialogtext(
    SeatDialogText *text, strbuf *dlg_text, int *width)
{
    const char *dlg_title = NULL;

    for (SeatDialogTextItem *item = text->items,
             *end = item + text->nitems; item < end; item++) {
        switch (item->type) {
          case SDT_PARA:
            put_fmt(dlg_text, "%s\n\n", item->text);
            break;
          case SDT_DISPLAY: {
            put_fmt(dlg_text, "%s\n\n", item->text);
            int thiswidth = string_width(item->text);
            if (*width < thiswidth)
                *width = thiswidth;
            break;
          }
          case SDT_SCARY_HEADING:
            /* Can't change font size or weight in this context */
            put_fmt(dlg_text, "%s\n\n", item->text);
            break;
          case SDT_TITLE:
            dlg_title = item->text;
            break;
          default:
            break;
        }
    }

    /*
     * Trim trailing newlines.
     */
    while (strbuf_chomp(dlg_text, '\n'));

    return dlg_title;
}

SeatPromptResult gtk_seat_confirm_ssh_host_key(
    Seat *seat, const char *host, int port, const char *keytype,
    char *keystr, SeatDialogText *text, HelpCtx helpctx,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    static const struct message_box_button button_array_hostkey[] = {
        {"Accept", 'a', 0, 2},
        {"Connect Once", 'o', 0, 1},
        {"Cancel", 'c', -1, 0},
    };
    static const struct message_box_buttons buttons_hostkey = {
        button_array_hostkey, lenof(button_array_hostkey),
    };

    int width = string_width("default dialog width determination string");
    strbuf *dlg_text = strbuf_new();
    const char *dlg_title = gtk_format_seatdialogtext(text, dlg_text, &width);

    GtkWidget *mainwin, *msgbox;

    struct confirm_ssh_host_key_dialog_ctx *result_ctx =
        snew(struct confirm_ssh_host_key_dialog_ctx);
    result_ctx->callback = callback;
    result_ctx->callback_ctx = ctx;
    result_ctx->host = dupstr(host);
    result_ctx->port = port;
    result_ctx->keytype = dupstr(keytype);
    result_ctx->keystr = dupstr(keystr);
    result_ctx->seat = seat;

    mainwin = GTK_WIDGET(gtk_seat_get_window(seat));
    GtkWidget *more_info_button = NULL;
    msgbox = create_message_box_general(
        mainwin, dlg_title, dlg_text->s, width, true,
        &buttons_hostkey, confirm_ssh_host_key_result_callback, result_ctx,
        add_more_info_button, &more_info_button);

    result_ctx->main_dialog = msgbox;
    result_ctx->more_info_dialog = NULL;

    strbuf *moreinfo = strbuf_new();
    for (SeatDialogTextItem *item = text->items,
             *end = item + text->nitems; item < end; item++) {
        switch (item->type) {
          case SDT_MORE_INFO_KEY:
            put_fmt(moreinfo, "%s", item->text);
            break;
          case SDT_MORE_INFO_VALUE_SHORT:
            put_fmt(moreinfo, ": %s\n", item->text);
            break;
          case SDT_MORE_INFO_VALUE_BLOB:
            /* We have to manually wrap the public key, or else the GtkLabel
             * will resize itself to accommodate the longest word, which will
             * lead to a hilariously wide message box. */
            put_byte(moreinfo, ':');
            for (const char *p = item->text, *q = p + strlen(p); p < q ;) {
                size_t linelen = q-p;
                if (linelen > 72)
                    linelen = 72;
                put_byte(moreinfo, '\n');
                put_data(moreinfo, p, linelen);
                p += linelen;
            }
            put_byte(moreinfo, '\n');
            break;
          default:
            break;
        }
    }
    result_ctx->more_info = strbuf_to_str(moreinfo);

    g_signal_connect(G_OBJECT(more_info_button), "clicked",
                     G_CALLBACK(more_info_button_clicked), result_ctx);

    register_dialog(seat, DIALOG_SLOT_NETWORK_PROMPT, msgbox);

    strbuf_free(dlg_text);

    return SPR_INCOMPLETE;             /* dialog still in progress */
}

struct simple_prompt_result_spr_ctx {
    void (*callback)(void *callback_ctx, SeatPromptResult spr);
    void *callback_ctx;
    Seat *seat;
    enum DialogSlot dialog_slot;
};

static void simple_prompt_result_spr_callback(void *vctx, int result)
{
    struct simple_prompt_result_spr_ctx *ctx =
        (struct simple_prompt_result_spr_ctx *)vctx;

    unregister_dialog(ctx->seat, ctx->dialog_slot);

    if (result == 0)
        ctx->callback(ctx->callback_ctx, SPR_USER_ABORT);
    else if (result > 0)
        ctx->callback(ctx->callback_ctx, SPR_OK);
    /* if <0, we're cleaning up for some other reason */

    /*
     * Clean up this context structure, whether or not a result was
     * ever actually delivered from the dialog box.
     */
    sfree(ctx);
}

/*
 * Ask whether the selected algorithm is acceptable (since it was
 * below the configured 'warn' threshold).
 */
SeatPromptResult gtk_seat_confirm_weak_crypto_primitive(
    Seat *seat, SeatDialogText *text,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    struct simple_prompt_result_spr_ctx *result_ctx;
    GtkWidget *mainwin, *msgbox;

    int width = string_width("Reasonably long line of text "
                             "as a width template");
    strbuf *dlg_text = strbuf_new();
    const char *dlg_title = gtk_format_seatdialogtext(text, dlg_text, &width);

    result_ctx = snew(struct simple_prompt_result_spr_ctx);
    result_ctx->callback = callback;
    result_ctx->callback_ctx = ctx;
    result_ctx->seat = seat;
    result_ctx->dialog_slot = DIALOG_SLOT_NETWORK_PROMPT;

    mainwin = GTK_WIDGET(gtk_seat_get_window(seat));
    msgbox = create_message_box(
        mainwin, dlg_title, dlg_text->s, width, false,
        &buttons_yn, simple_prompt_result_spr_callback, result_ctx);
    register_dialog(seat, result_ctx->dialog_slot, msgbox);

    strbuf_free(dlg_text);

    return SPR_INCOMPLETE;
}

SeatPromptResult gtk_seat_confirm_weak_cached_hostkey(
    Seat *seat, SeatDialogText *text,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    struct simple_prompt_result_spr_ctx *result_ctx;
    GtkWidget *mainwin, *msgbox;

    int width = string_width("is ecdsa-nistp521, which is below the configured"
                             " warning threshold.");
    strbuf *dlg_text = strbuf_new();
    const char *dlg_title = gtk_format_seatdialogtext(text, dlg_text, &width);

    result_ctx = snew(struct simple_prompt_result_spr_ctx);
    result_ctx->callback = callback;
    result_ctx->callback_ctx = ctx;
    result_ctx->seat = seat;
    result_ctx->dialog_slot = DIALOG_SLOT_NETWORK_PROMPT;

    mainwin = GTK_WIDGET(gtk_seat_get_window(seat));
    msgbox = create_message_box(
        mainwin, dlg_title, dlg_text->s, width, false,
        &buttons_yn, simple_prompt_result_spr_callback, result_ctx);
    register_dialog(seat, result_ctx->dialog_slot, msgbox);

    strbuf_free(dlg_text);

    return SPR_INCOMPLETE;
}

void old_keyfile_warning(void)
{
    /*
     * This should never happen on Unix. We hope.
     */
}

void nonfatal_message_box(void *window, const char *msg)
{
    char *title = dupcat(appname, " Error");
    create_message_box(
        window, title, msg,
        string_width("REASONABLY LONG LINE OF TEXT FOR BASIC SANITY"),
        false, &buttons_ok, trivial_post_dialog_fn, NULL);
    sfree(title);
}

void nonfatal(const char *p, ...)
{
    va_list ap;
    char *msg;
    va_start(ap, p);
    msg = dupvprintf(p, ap);
    va_end(ap);
    nonfatal_message_box(NULL, msg);
    sfree(msg);
}

static GtkWidget *aboutbox = NULL;

static void about_window_destroyed(GtkWidget *widget, gpointer data)
{
    aboutbox = NULL;
}

static void about_close_clicked(GtkButton *button, gpointer data)
{
    gtk_widget_destroy(aboutbox);
    aboutbox = NULL;
}

static void about_key_press(GtkWidget *widget, GdkEventKey *event,
                            gpointer data)
{
    if (event->keyval == GDK_KEY_Escape && aboutbox) {
        gtk_widget_destroy(aboutbox);
        aboutbox = NULL;
    }
}

static void licence_clicked(GtkButton *button, gpointer data)
{
    char *title;

    title = dupcat(appname, " Licence");
    assert(aboutbox != NULL);
    create_message_box(aboutbox, title, LICENCE_TEXT("\n\n"),
                       string_width("LONGISH LINE OF TEXT SO THE LICENCE"
                                    " BOX ISN'T EXCESSIVELY TALL AND THIN"),
                       true, &buttons_ok, trivial_post_dialog_fn, NULL);
    sfree(title);
}

void about_box(void *window)
{
    GtkWidget *w;
    GtkBox *action_area;
    char *title;

    if (aboutbox) {
        gtk_widget_grab_focus(aboutbox);
        return;
    }

    aboutbox = our_dialog_new();
    gtk_container_set_border_width(GTK_CONTAINER(aboutbox), 10);
    title = dupcat("About ", appname);
    gtk_window_set_title(GTK_WINDOW(aboutbox), title);
    sfree(title);

    g_signal_connect(G_OBJECT(aboutbox), "destroy",
                     G_CALLBACK(about_window_destroyed), NULL);

    w = gtk_button_new_with_label("Close");
    gtk_widget_set_can_default(w, true);
    gtk_window_set_default(GTK_WINDOW(aboutbox), w);
    action_area = our_dialog_make_action_hbox(GTK_WINDOW(aboutbox));
    gtk_box_pack_end(action_area, w, false, false, 0);
    g_signal_connect(G_OBJECT(w), "clicked",
                     G_CALLBACK(about_close_clicked), NULL);
    gtk_widget_show(w);

    w = gtk_button_new_with_label("View Licence");
    gtk_widget_set_can_default(w, true);
    gtk_box_pack_end(action_area, w, false, false, 0);
    g_signal_connect(G_OBJECT(w), "clicked",
                     G_CALLBACK(licence_clicked), NULL);
    gtk_widget_show(w);

    {
        char *buildinfo_text = buildinfo("\n");
        char *label_text = dupprintf(
            "%s\n\n%s\n\n%s\n\n%s",
            appname, ver, buildinfo_text,
            "Copyright " SHORT_COPYRIGHT_DETAILS ". All rights reserved");
        w = gtk_label_new(label_text);
        gtk_label_set_justify(GTK_LABEL(w), GTK_JUSTIFY_CENTER);
#if GTK_CHECK_VERSION(2,0,0)
        gtk_label_set_selectable(GTK_LABEL(w), true);
#endif
        sfree(label_text);
    }
    our_dialog_add_to_content_area(GTK_WINDOW(aboutbox), w, false, false, 0);
#if GTK_CHECK_VERSION(2,0,0)
    /*
     * Same precautions against initial select-all as in
     * create_message_box().
     */
    gtk_widget_grab_focus(w);
    gtk_label_select_region(GTK_LABEL(w), 0, 0);
#endif
    gtk_widget_show(w);

    g_signal_connect(G_OBJECT(aboutbox), "key_press_event",
                     G_CALLBACK(about_key_press), NULL);

    set_transient_window_pos(GTK_WIDGET(window), aboutbox);
    if (window)
        gtk_window_set_transient_for(GTK_WINDOW(aboutbox),
                                     GTK_WINDOW(window));
    gtk_container_set_focus_child(GTK_CONTAINER(aboutbox), NULL);
    gtk_widget_show(aboutbox);
    gtk_window_set_focus(GTK_WINDOW(aboutbox), NULL);
}

#define LOGEVENT_INITIAL_MAX 128
#define LOGEVENT_CIRCULAR_MAX 128

struct eventlog_stuff {
    GtkWidget *parentwin, *window;
    struct controlbox *eventbox;
    struct Shortcuts scs;
    struct dlgparam dp;
    dlgcontrol *listctrl;
    char **events_initial;
    char **events_circular;
    int ninitial, ncircular, circular_first;
    strbuf *seldata;
    int sellen;
    bool ignore_selchange;
};

static void eventlog_destroy(GtkWidget *widget, gpointer data)
{
    eventlog_stuff *es = (eventlog_stuff *)data;

    es->window = NULL;
    dlg_cleanup(&es->dp);
    ctrl_free_box(es->eventbox);
}
static void eventlog_ok_handler(dlgcontrol *ctrl, dlgparam *dp,
                                void *data, int event)
{
    if (event == EVENT_ACTION)
        dlg_end(dp, 0);
}
static void eventlog_list_handler(dlgcontrol *ctrl, dlgparam *dp,
                                  void *data, int event)
{
    eventlog_stuff *es = (eventlog_stuff *)data;

    if (event == EVENT_REFRESH) {
        int i;

        dlg_update_start(ctrl, dp);
        dlg_listbox_clear(ctrl, dp);
        for (i = 0; i < es->ninitial; i++) {
            dlg_listbox_add(ctrl, dp, es->events_initial[i]);
        }
        for (i = 0; i < es->ncircular; i++) {
            dlg_listbox_add(ctrl, dp, es->events_circular[(es->circular_first + i) % LOGEVENT_CIRCULAR_MAX]);
        }
        dlg_update_done(ctrl, dp);
    } else if (event == EVENT_SELCHANGE) {
        int i;

        /*
         * If this SELCHANGE event is happening as a result of
         * deliberate deselection because someone else has grabbed
         * the selection, the last thing we want to do is pre-empt
         * them.
         */
        if (es->ignore_selchange)
            return;

        /*
         * Construct the data to use as the selection.
         */
        strbuf_clear(es->seldata);
        for (i = 0; i < es->ninitial; i++) {
            if (dlg_listbox_issel(ctrl, dp, i))
                put_fmt(es->seldata, "%s\n", es->events_initial[i]);
        }
        for (i = 0; i < es->ncircular; i++) {
            if (dlg_listbox_issel(ctrl, dp, es->ninitial + i)) {
                int j = (es->circular_first + i) % LOGEVENT_CIRCULAR_MAX;
                put_fmt(es->seldata, "%s\n", es->events_circular[j]);
            }
        }

        if (gtk_selection_owner_set(es->window, GDK_SELECTION_PRIMARY,
                                    gtk_get_current_event_time())) {
            gtk_selection_add_target(es->window, GDK_SELECTION_PRIMARY,
                                     GDK_SELECTION_TYPE_STRING, 1);
            gtk_selection_add_target(es->window, GDK_SELECTION_PRIMARY,
                                     compound_text_atom, 1);
        }

    }
}

void eventlog_selection_get(GtkWidget *widget, GtkSelectionData *seldata,
                            guint info, guint time_stamp, gpointer data)
{
    eventlog_stuff *es = (eventlog_stuff *)data;

    gtk_selection_data_set(seldata, gtk_selection_data_get_target(seldata), 8,
                           es->seldata->u, es->seldata->len);
}

gint eventlog_selection_clear(GtkWidget *widget, GdkEventSelection *seldata,
                              gpointer data)
{
    eventlog_stuff *es = (eventlog_stuff *)data;
    struct uctrl *uc;

    /*
     * Deselect everything in the list box.
     */
    uc = dlg_find_byctrl(&es->dp, es->listctrl);
    es->ignore_selchange = true;
#if !GTK_CHECK_VERSION(2,0,0)
    assert(uc->list);
    gtk_list_unselect_all(GTK_LIST(uc->list));
#else
    assert(uc->treeview);
    gtk_tree_selection_unselect_all(
        gtk_tree_view_get_selection(GTK_TREE_VIEW(uc->treeview)));
#endif
    es->ignore_selchange = false;

    return true;
}

void showeventlog(eventlog_stuff *es, void *parentwin)
{
    GtkWidget *window, *w0, *w1;
    GtkWidget *parent = GTK_WIDGET(parentwin);
    struct controlset *s0, *s1;
    dlgcontrol *c;
    int index;
    char *title;

    if (es->window) {
        gtk_widget_grab_focus(es->window);
        return;
    }

    dlg_init(&es->dp);

    for (index = 0; index < lenof(es->scs.sc); index++) {
        es->scs.sc[index].action = SHORTCUT_EMPTY;
    }

    es->eventbox = ctrl_new_box();

    s0 = ctrl_getset(es->eventbox, "", "", "");
    ctrl_columns(s0, 3, 33, 34, 33);
    c = ctrl_pushbutton(s0, "Close", 'c', HELPCTX(no_help),
                        eventlog_ok_handler, P(NULL));
    c->column = 1;
    c->button.isdefault = true;

    s1 = ctrl_getset(es->eventbox, "x", "", "");
    es->listctrl = c = ctrl_listbox(s1, NULL, NO_SHORTCUT, HELPCTX(no_help),
                                    eventlog_list_handler, P(es));
    c->listbox.height = 10;
    c->listbox.multisel = 2;
    c->listbox.ncols = 3;
    c->listbox.percentages = snewn(3, int);
    c->listbox.percentages[0] = 25;
    c->listbox.percentages[1] = 10;
    c->listbox.percentages[2] = 65;

    es->window = window = our_dialog_new();
    title = dupcat(appname, " Event Log");
    gtk_window_set_title(GTK_WINDOW(window), title);
    sfree(title);
    w0 = layout_ctrls(&es->dp, NULL, &es->scs, s0, GTK_WINDOW(window));
    our_dialog_set_action_area(GTK_WINDOW(window), w0);
    gtk_widget_show(w0);
    w1 = layout_ctrls(&es->dp, NULL, &es->scs, s1, GTK_WINDOW(window));
    gtk_container_set_border_width(GTK_CONTAINER(w1), 10);
    gtk_widget_set_size_request(
        w1, 20 + string_width("LINE OF TEXT GIVING WIDTH OF EVENT LOG IS "
                              "QUITE LONG 'COS SSH LOG ENTRIES ARE WIDE"),
        -1);
    our_dialog_add_to_content_area(GTK_WINDOW(window), w1, true, true, 0);
    {
        struct uctrl *uc = dlg_find_byctrl(&es->dp, es->listctrl);
        columns_vexpand(COLUMNS(w1), uc->toplevel);
    }
    gtk_widget_show(w1);

    es->dp.data = es;
    es->dp.shortcuts = &es->scs;
    es->dp.lastfocus = NULL;
    es->dp.retval = 0;
    es->dp.window = window;

    dlg_refresh(NULL, &es->dp);

    if (parent) {
        set_transient_window_pos(parent, window);
        gtk_window_set_transient_for(GTK_WINDOW(window),
                                     GTK_WINDOW(parent));
    } else
        gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    gtk_widget_show(window);

    g_signal_connect(G_OBJECT(window), "destroy",
                     G_CALLBACK(eventlog_destroy), es);
    g_signal_connect(G_OBJECT(window), "key_press_event",
                     G_CALLBACK(win_key_press), &es->dp);
    g_signal_connect(G_OBJECT(window), "selection_get",
                     G_CALLBACK(eventlog_selection_get), es);
    g_signal_connect(G_OBJECT(window), "selection_clear_event",
                     G_CALLBACK(eventlog_selection_clear), es);
}

eventlog_stuff *eventlogstuff_new(void)
{
    eventlog_stuff *es = snew(eventlog_stuff);
    memset(es, 0, sizeof(*es));
    es->seldata = strbuf_new();
    return es;
}

void eventlogstuff_free(eventlog_stuff *es)
{
    int i;

    if (es->events_initial) {
        for (i = 0; i < LOGEVENT_INITIAL_MAX; i++)
            sfree(es->events_initial[i]);
        sfree(es->events_initial);
    }
    if (es->events_circular) {
        for (i = 0; i < LOGEVENT_CIRCULAR_MAX; i++)
            sfree(es->events_circular[i]);
        sfree(es->events_circular);
    }
    strbuf_free(es->seldata);

    sfree(es);
}

void logevent_dlg(eventlog_stuff *es, const char *string)
{
    char timebuf[40];
    struct tm tm;
    char **location;
    size_t i;

    if (es->ninitial == 0) {
        es->events_initial = sresize(es->events_initial, LOGEVENT_INITIAL_MAX, char *);
        for (i = 0; i < LOGEVENT_INITIAL_MAX; i++)
            es->events_initial[i] = NULL;
        es->events_circular = sresize(es->events_circular, LOGEVENT_CIRCULAR_MAX, char *);
        for (i = 0; i < LOGEVENT_CIRCULAR_MAX; i++)
            es->events_circular[i] = NULL;
    }

    if (es->ninitial < LOGEVENT_INITIAL_MAX)
        location = &es->events_initial[es->ninitial];
    else
        location = &es->events_circular[(es->circular_first + es->ncircular) % LOGEVENT_CIRCULAR_MAX];

    tm=ltime();
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S\t", &tm);

    sfree(*location);
    *location = dupcat(timebuf, string);
    if (es->window) {
        dlg_listbox_add(es->listctrl, &es->dp, *location);
    }
    if (es->ninitial < LOGEVENT_INITIAL_MAX) {
        es->ninitial++;
    } else if (es->ncircular < LOGEVENT_CIRCULAR_MAX) {
        es->ncircular++;
    } else if (es->ncircular == LOGEVENT_CIRCULAR_MAX) {
        es->circular_first = (es->circular_first + 1) % LOGEVENT_CIRCULAR_MAX;
        sfree(es->events_circular[es->circular_first]);
        es->events_circular[es->circular_first] = dupstr("..");
    }
}

struct simple_prompt_result_int_ctx {
    void (*callback)(void *callback_ctx, int result);
    void *callback_ctx;
    Seat *seat;
    enum DialogSlot dialog_slot;
};

static void simple_prompt_result_int_callback(void *vctx, int result)
{
    struct simple_prompt_result_int_ctx *ctx =
        (struct simple_prompt_result_int_ctx *)vctx;

    unregister_dialog(ctx->seat, ctx->dialog_slot);

    if (result >= 0)
        ctx->callback(ctx->callback_ctx, result);

    /*
     * Clean up this context structure, whether or not a result was
     * ever actually delivered from the dialog box.
     */
    sfree(ctx);
}

int gtkdlg_askappend(Seat *seat, Filename *filename,
                     void (*callback)(void *ctx, int result), void *ctx)
{
    static const char msgtemplate[] =
        "The session log file \"%.*s\" already exists. "
        "You can overwrite it with a new session log, "
        "append your session log to the end of it, "
        "or disable session logging for this session.";
    static const struct message_box_button button_array_append[] = {
        {"Overwrite", 'o', 1, 2},
        {"Append", 'a', 0, 1},
        {"Disable", 'd', -1, 0},
    };
    static const struct message_box_buttons buttons_append = {
        button_array_append, lenof(button_array_append),
    };

    char *message;
    char *mbtitle;
    struct simple_prompt_result_int_ctx *result_ctx;
    GtkWidget *mainwin, *msgbox;

    message = dupprintf(msgtemplate, FILENAME_MAX, filename->path);
    mbtitle = dupprintf("%s Log to File", appname);

    result_ctx = snew(struct simple_prompt_result_int_ctx);
    result_ctx->callback = callback;
    result_ctx->callback_ctx = ctx;
    result_ctx->seat = seat;
    result_ctx->dialog_slot = DIALOG_SLOT_LOGFILE_PROMPT;

    mainwin = GTK_WIDGET(gtk_seat_get_window(seat));
    msgbox = create_message_box(
        mainwin, mbtitle, message,
        string_width("LINE OF TEXT SUITABLE FOR THE ASKAPPEND WIDTH"),
        false, &buttons_append, simple_prompt_result_int_callback, result_ctx);
    register_dialog(seat, result_ctx->dialog_slot, msgbox);

    sfree(message);
    sfree(mbtitle);

    return -1;                         /* dialog still in progress */
}

struct ca_config_box {
    GtkWidget *window;
    struct controlbox *cb;
    struct Shortcuts scs;
    bool quit_main;
    dlgparam dp;
};
static struct ca_config_box *cacfg;    /* one of these, cross-instance */

static void cacfg_destroy(GtkWidget *widget, gpointer data)
{
    cacfg->window = NULL;
    dlg_cleanup(&cacfg->dp);
    ctrl_free_box(cacfg->cb);
    cacfg->cb = NULL;
    if (cacfg->quit_main)
        gtk_main_quit();
}
static void make_ca_config_box(GtkWidget *spawning_window)
{
    if (!cacfg) {
        cacfg = snew(struct ca_config_box);
        memset(cacfg, 0, sizeof(*cacfg));
    }

    if (cacfg->window) {
        /* This dialog box is already displayed; re-focus it */
        gtk_widget_grab_focus(cacfg->window);
        return;
    }

    dlg_init(&cacfg->dp);
    for (size_t i = 0; i < lenof(cacfg->scs.sc); i++) {
        cacfg->scs.sc[i].action = SHORTCUT_EMPTY;
    }

    cacfg->cb = ctrl_new_box();
    setup_ca_config_box(cacfg->cb);

    cacfg->window = our_dialog_new();
    gtk_window_set_title(GTK_WINDOW(cacfg->window),
                         "PuTTY trusted host certification authorities");
    gtk_widget_set_size_request(
        cacfg->window, string_width(
            "ecdsa-sha2-nistp256 256 SHA256:hsO5a8MYGzBoa2gW5"
            "dLV2vl7bTnCPjw64x3kLkz6BY8"), -1);

    /* Set up everything else */
    for (int i = 0; i < cacfg->cb->nctrlsets; i++) {
        struct controlset *s = cacfg->cb->ctrlsets[i];
        GtkWidget *w = layout_ctrls(&cacfg->dp, NULL, &cacfg->scs, s,
                                    GTK_WINDOW(cacfg->window));
        gtk_container_set_border_width(GTK_CONTAINER(w), 10);
        gtk_widget_show(w);

        if (!*s->pathname) {
            our_dialog_set_action_area(GTK_WINDOW(cacfg->window), w);
        } else {
            our_dialog_add_to_content_area(GTK_WINDOW(cacfg->window), w,
                                           true, true, 0);
        }
    }

    cacfg->dp.data = cacfg;
    cacfg->dp.shortcuts = &cacfg->scs;
    cacfg->dp.lastfocus = NULL;
    cacfg->dp.retval = 0;
    cacfg->dp.window = cacfg->window;

    dlg_refresh(NULL, &cacfg->dp);

    if (spawning_window) {
        set_transient_window_pos(spawning_window, cacfg->window);
    } else {
        gtk_window_set_position(GTK_WINDOW(cacfg->window), GTK_WIN_POS_CENTER);
    }
    gtk_widget_show(cacfg->window);

    g_signal_connect(G_OBJECT(cacfg->window), "destroy",
                     G_CALLBACK(cacfg_destroy), NULL);
    g_signal_connect(G_OBJECT(cacfg->window), "key_press_event",
                     G_CALLBACK(win_key_press), &cacfg->dp);
}

void show_ca_config_box(dlgparam *dp)
{
    make_ca_config_box(dp ? dp->window : NULL);
}

void show_ca_config_box_synchronously(void)
{
    make_ca_config_box(NULL);
    cacfg->quit_main = true;
    gtk_main();
}
