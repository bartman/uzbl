// Original code taken from the example webkit-gtk+ application. see notice below.
// Modified code is licensed under the GPL 3.  See LICENSE file.


/*
 * Copyright (C) 2006, 2007 Apple Inc.
 * Copyright (C) 2007 Alp Toker <alp@atoker.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#define LENGTH(x)               (sizeof x / sizeof x[0])

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <webkit/webkit.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>

static GtkWidget* main_window;
static GtkWidget* uri_entry;
static GtkWidget* mainbar;
static WebKitWebView* web_view;
static gchar* main_title;

/* Behaviour variables */
static gchar* history_file       = NULL;
static gchar* fifodir            = NULL;
static gchar* download_handler   = NULL;
static gchar* always_insert_mode = NULL;
static gchar* modkey             = NULL;

static char fifopath[64];
static gint load_progress;
static guint status_context_id;
static Window xwin = 0;
static gchar* uri = NULL;

static gboolean verbose = FALSE;

static GOptionEntry entries[] =
{
  { "uri",     'u', 0, G_OPTION_ARG_STRING, &uri,     "Uri to load", NULL },
  { "verbose", 'v', 0, G_OPTION_ARG_NONE,   &verbose, "Be verbose",  NULL },
  { NULL }
};

typedef struct
{
  const char *command;
  void (*func)(WebKitWebView*);
  const gboolean param_allow;
  const gboolean param_optional;
} Command;

typedef struct
{
  const char *binding;
  const char *action;
} Binding;

static Binding internal_bindings[256];
static Binding external_bindings[256];
static int     num_internal_bindings = 0;
static int     num_external_bindings = 0;

static void
update_title (GtkWindow* window);


/* --- CALLBACKS --- */
static void
go_back_cb (GtkWidget* widget, gpointer data) {
    webkit_web_view_go_back (web_view);
}

static void
go_forward_cb (GtkWidget* widget, gpointer data) {
    webkit_web_view_go_forward (web_view);
}


static void
link_hover_cb (WebKitWebView* page, const gchar* title, const gchar* link, gpointer data) {
    /* underflow is allowed */
    //gtk_statusbar_pop (main_statusbar, status_context_id);
    //if (link)
    //    gtk_statusbar_push (main_statusbar, status_context_id, link);
    //TODO implementation roadmap pending..
}

static void
title_change_cb (WebKitWebView* web_view, WebKitWebFrame* web_frame, const gchar* title, gpointer data) {
    if (main_title)
        g_free (main_title);
    main_title = g_strdup (title);
    update_title (GTK_WINDOW (main_window));
}

static void
progress_change_cb (WebKitWebView* page, gint progress, gpointer data) {
    load_progress = progress;
    update_title (GTK_WINDOW (main_window));
}

static void
load_commit_cb (WebKitWebView* page, WebKitWebFrame* frame, gpointer data) {
    const gchar* uri = webkit_web_frame_get_uri(frame);
    if (uri)
        gtk_entry_set_text (GTK_ENTRY (uri_entry), uri);
}

static void
destroy_cb (GtkWidget* widget, gpointer data) {
    gtk_main_quit ();
}

static void
activate_uri_entry_cb (GtkWidget* entry, gpointer data) {
    const gchar * uri = gtk_entry_get_text (GTK_ENTRY (entry));
    g_assert (uri);
    webkit_web_view_load_uri (web_view, uri);
}

static void
log_history_cb () {
    FILE * output_file = fopen(history_file, "a");
    if (output_file == NULL) {
       fprintf(stderr, "Cannot open %s for logging\n", history_file);
    } else {
        time_t rawtime;
        struct tm * timeinfo;
        char buffer [80];
        time ( &rawtime );
        timeinfo = localtime ( &rawtime );
        strftime (buffer,80,"%Y-%m-%d %H:%M:%S",timeinfo);

        fprintf(output_file, "%s %s\n",buffer, uri);
        fclose(output_file);
    }
}

/* -- command to callback/function map for things we cannot attach to any signals */
// TODO: reload, home, quit
static Command commands[] =
{
  { "back",     &go_back_cb,                    false, false },
  { "forward",  &go_forward_cb,                 false, false },
  { "refresh",  &webkit_web_view_reload,        false, false }, //Buggy
  { "stop",     &webkit_web_view_stop_loading,  false, false },
  { "zoom_in",  &webkit_web_view_zoom_in,       false, false }, //Can crash (when max zoom reached?).
  { "zoom_out", &webkit_web_view_zoom_out,      false, false } ,
  { "go",       &webkit_web_view_load_uri,      true,  false }
//{ "get uri",  &webkit_web_view_get_uri},
};

/* -- CORE FUNCTIONS -- */
 
static void
parse_command(const char *command) {
    int  i    = 0;
    bool done = false;
    void (*func)(WebKitWebView*);

    char * command_name = strtok (command," ");
    char * command_param = strtok (NULL, " ,"); //dunno how this works, but it seems to work

    Command *c;
    for (i = 0; i < LENGTH(commands); i++) {
        c = &commands[i];
        if (!strncmp (command_name, c->command, strlen (c->command))) {
            func = c->func;
            done = true;
        }
    }

    if (done) {
        if (c->param_allow) {
            if(command_param != NULL) {
                printf("command executing: \"%s %s\"\n", command_name, command_param);
                //func (web_view, command_param);
            } else {
                if(c->param_optional) {
                    printf("command executing: \"%s\"\n", command_name);
                    func (web_view);
                } else {
                    fprintf(stderr, "command needs a parameter. \"%s\" is not complete\n", command_name);
                }
            }
        } else {
            printf("command executing: \"%s\"\n", command_name);
            func (web_view);
        }
    } else {
        fprintf(stderr, "command \"%s\" not understood. ignoring.\n", command);
    }
}
 
static void
*control_fifo() {
  if (fifodir) {
    sprintf (fifopath, "%s/uzbl_%d", fifodir, (int) xwin);
  } else {
    sprintf (fifopath, "/tmp/uzbl_%d", (int) xwin);
  }
 
  if (mkfifo (fifopath, 0666) == -1) {
    printf ("Possible error creating fifo\n");
  }
 
  printf ("Control fifo opened in %s\n", fifopath);
 
  while (true) {
    FILE *fifo = fopen(fifopath, "r");
    if (!fifo) {
      printf("Could not open %s for reading\n", fifopath);
      return NULL;
    }
        
    char buffer[256];
    memset (buffer, 0, sizeof (buffer));
    while (!feof (fifo) && fgets (buffer, sizeof (buffer), fifo)) {
      if (strcmp (buffer, "\n")) {
        buffer[strlen (buffer) - 1] = '\0'; // Remove newline
        parse_command (buffer);
      }
    }
  }
    
  return NULL;
}
 
 
static void
setup_threading () {
  pthread_t control_thread;
  pthread_create(&control_thread, NULL, control_fifo, NULL);
}




static void
update_title (GtkWindow* window) {
    GString* string = g_string_new (main_title);
    g_string_append (string, " - Uzbl browser");
    if (load_progress < 100)
        g_string_append_printf (string, " (%d%%)", load_progress);
    gchar* title = g_string_free (string, FALSE);
    gtk_window_set_title (window, title);
    g_free (title);
}

static GtkWidget*
create_browser () {
    GtkWidget* scrolled_window = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled_window), GTK_POLICY_NEVER, GTK_POLICY_NEVER); //todo: some sort of display of position/total length. like what emacs does

    web_view = WEBKIT_WEB_VIEW (webkit_web_view_new ());
    gtk_container_add (GTK_CONTAINER (scrolled_window), GTK_WIDGET (web_view));

    g_signal_connect (G_OBJECT (web_view), "title-changed", G_CALLBACK (title_change_cb), web_view);
    g_signal_connect (G_OBJECT (web_view), "load-progress-changed", G_CALLBACK (progress_change_cb), web_view);
    g_signal_connect (G_OBJECT (web_view), "load-committed", G_CALLBACK (load_commit_cb), web_view);
    g_signal_connect (G_OBJECT (web_view), "load-committed", G_CALLBACK (log_history_cb), web_view);
    g_signal_connect (G_OBJECT (web_view), "hovering-over-link", G_CALLBACK (link_hover_cb), web_view);

    return scrolled_window;
}

static GtkWidget*
create_mainbar () {
    mainbar = gtk_hbox_new(FALSE, 0);
    uri_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(uri_entry), 40);
    gtk_entry_set_text(GTK_ENTRY(uri_entry), "http://");
    gtk_box_pack_start (GTK_BOX (mainbar), uri_entry, TRUE,TRUE , 0);
    gtk_signal_connect_object (GTK_OBJECT (uri_entry), "activate", GTK_SIGNAL_FUNC (activate_uri_entry_cb), GTK_OBJECT (uri_entry));

    //status_context_id = gtk_statusbar_get_context_id (main_statusbar, "Link Hover");

    return mainbar;
}

static
GtkWidget* create_window () {
    GtkWidget* window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size (GTK_WINDOW (window), 800, 600);
    gtk_widget_set_name (window, "Uzbl browser");
    g_signal_connect (G_OBJECT (window), "destroy", G_CALLBACK (destroy_cb), NULL);

    return window;
}

static void
add_binding (char *binding, char *action, bool internal) {
    Binding bind = {binding, action};
    if (internal) {
        internal_bindings[num_internal_bindings] = bind;
        num_internal_bindings ++;
    } else {
        external_bindings[num_external_bindings] = bind;
        num_external_bindings ++;
    }
}

static void
settings_init () {
    GKeyFile* config = g_key_file_new ();
    gboolean res = g_key_file_load_from_file (config, "./sampleconfig", G_KEY_FILE_NONE, NULL); //TODO: pass config file as argument
    if (res) {
        printf ("Config loaded\n");
    } else {
        fprintf (stderr, "Config loading failed\n"); //TODO: exit codes with gtk? 
    }

    history_file = g_key_file_get_value (config, "behavior", "history_file", NULL);
    if (history_file) {
        printf ("History file: %s\n", history_file);
    } else {
        printf ("History logging disabled\n");
    }

    download_handler = g_key_file_get_value (config, "behavior", "download_handler", NULL);
    if (download_handler) {
        printf ("Download manager: %s\n", history_file);
    } else {
        printf ("Download manager disabled\n");
    }

    if (! fifodir)
        fifodir = g_key_file_get_value (config, "behavior", "fifodir", NULL);
    if (fifodir) {
        printf ("Fifo directory: %s\n", history_file);
    } else {
        printf ("Fifo directory: /tmp\n");
    }

    always_insert_mode = g_key_file_get_value (config, "behavior", "always_insert_mode", NULL);
    if (always_insert_mode) {
        printf ("Always insert mode: %s\n", history_file);
    } else {
        printf ("Always insert mode disabled/\n");
    }

    modkey = g_key_file_get_value (config, "behavior", "modkey", NULL);
    if (modkey) {
        printf ("Mod key: %s\n", history_file);
    } else {
        printf ("Mod key disabled/\n");
    }

    gchar **keysi = g_key_file_get_keys (config, "bindings_internal", NULL, NULL);
    int i = 0;
    for (i = 0; keysi[i]; i++)
      {
        gchar *binding = g_key_file_get_string(config, "bindings_internal", keysi[i], NULL);
        printf("Action: %s, Binding: %s (internal)\n", g_strdup (keysi[i]), binding);
        add_binding (binding, g_strdup (keysi[i]), true);
      }

    gchar **keyse = g_key_file_get_keys (config, "bindings_external", NULL, NULL);
    for (i = 0; keyse[i]; i++)
      {
        gchar *binding = g_key_file_get_string(config, "bindings_external", keyse[i], NULL);
        printf("Action: %s, Binding: %s (external)\n", g_strdup (keyse[i]), binding);
        add_binding (binding, g_strdup (keyse[i]), false);
      }
}

int
main (int argc, char* argv[]) {
    gtk_init (&argc, &argv);
    if (!g_thread_supported ())
        g_thread_init (NULL);

    settings_init ();

    GtkWidget* vbox = gtk_vbox_new (FALSE, 0);
    gtk_box_pack_start (GTK_BOX (vbox), create_mainbar (), FALSE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (vbox), create_browser (), TRUE, TRUE, 0);

    main_window = create_window ();
    gtk_container_add (GTK_CONTAINER (main_window), vbox);
    GError *error = NULL;

    GOptionContext* context = g_option_context_new ("- some stuff here maybe someday");
    g_option_context_add_main_entries (context, entries, NULL);
    g_option_context_add_group (context, gtk_get_option_group (TRUE));
    g_option_context_parse (context, &argc, &argv, &error);

    webkit_web_view_load_uri (web_view, uri);

    gtk_widget_grab_focus (GTK_WIDGET (web_view));
    gtk_widget_show_all (main_window);
    xwin = GDK_WINDOW_XID (GTK_WIDGET (main_window)->window);
    printf("window_id %i\n",(int) xwin);
    printf("pid %i\n", getpid ());

    setup_threading ();

    gtk_main ();

    unlink (fifopath);
    return 0;
}
