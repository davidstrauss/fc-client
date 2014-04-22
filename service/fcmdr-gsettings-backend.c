/*
 * Copyright (C) 2014 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the licence, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Matthew Barnes <mbarnes@redhat.com>
 */

#include "config.h"

#include "fcmdr-gsettings-backend.h"

#include <errno.h>
#include <string.h>

#include "fcmdr-extensions.h"
#include "fcmdr-utils.h"

#define FCMDR_GSETTINGS_BACKEND_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), FCMDR_TYPE_GSETTINGS_BACKEND, FCmdrGSettingsBackendPrivate))

#define FCMDR_GSETTINGS_PREAMBLE \
	"# Generated by " G_LOG_DOMAIN ". DO NOT EDIT.\n\n"

struct _FCmdrGSettingsBackendPrivate {
	GKeyFile *key_file;
	GHashTable *locks;

	gchar *sysdb_name;
	gchar *sysdb_path;
};

G_DEFINE_TYPE_WITH_CODE (
	FCmdrGSettingsBackend,
	fcmdr_gsettings_backend,
	FCMDR_TYPE_SETTINGS_BACKEND,
	fcmdr_ensure_extension_points_registered();
	g_io_extension_point_implement (
		FCMDR_SETTINGS_BACKEND_EXTENSION_POINT_NAME,
		g_define_type_id,
		"org.gnome.gsettings", 0))

static gboolean
fcmdr_gsettings_backend_write_key_file (FCmdrGSettingsBackend *backend,
                                        const gchar *filename,
                                        GError **error)
{
	GString *contents;
	gchar *data;
	gsize length = 0;
	gboolean success;

	contents = g_string_new (FCMDR_GSETTINGS_PREAMBLE);

	data = g_key_file_to_data (backend->priv->key_file, &length, NULL);
	g_string_append_len (contents, data, length);
	g_free (data);

	success = g_file_set_contents (
		filename, contents->str, contents->len, error);

	g_string_free (contents, TRUE);

	return success;
}

static gboolean
fcmdr_gsettings_backend_write_locks (FCmdrGSettingsBackend *backend,
                                     const gchar *filename,
                                     GError **error)
{
	GString *contents;
	GList *list, *link;
	gboolean success;

	contents = g_string_new (FCMDR_GSETTINGS_PREAMBLE);

	list = g_hash_table_get_keys (backend->priv->locks);
	list = g_list_sort (list, (GCompareFunc) strcmp);

	for (link = list; link != NULL; link = g_list_next (link)) {
		g_string_append (contents, link->data);
		g_string_append_c (contents, '\n');
	}

	g_list_free (list);

	success = g_file_set_contents (
		filename, contents->str, contents->len, error);

	g_string_free (contents, TRUE);

	return success;
}

static void
fcmdr_gsettings_backend_foreach_element_cb (JsonArray *json_array,
                                            guint index,
                                            JsonNode *json_node,
                                            gpointer user_data)
{
	GKeyFile *key_file = user_data;
	JsonObject *json_object;
	GVariant *value = NULL;
	const gchar *path;
	gchar *group;
	gchar *key;

	g_return_if_fail (JSON_NODE_HOLDS_OBJECT (json_node));

	json_object = json_node_get_object (json_node);
	g_return_if_fail (json_object_has_member (json_object, "key"));
	g_return_if_fail (json_object_has_member (json_object, "value"));

	path = json_object_get_string_member (json_object, "key");
	g_return_if_fail (path != NULL);

	/* Skip leading slashes. */
	while (*path == '/')
		path++;

	group = g_strdup (path);
	key = strrchr (group, '/');

	if (key != NULL)
		*key++ = '\0';

	json_node = json_object_get_member (json_object, "value");
	value = fcmdr_json_value_to_variant (json_node);

	/* This is how GKeyfileSettingsBackend does it. */
	if (key != NULL && value != NULL) {
		gchar *str = g_variant_print (value, FALSE);
		g_key_file_set_value (key_file, group, key, str);
		g_variant_unref (g_variant_ref_sink (value));
		g_free (str);
	}

	g_free (group);
}

static void
fcmdr_gsettings_backend_parse_json_node (FCmdrGSettingsBackend *backend,
                                         JsonNode *json_node)
{
	JsonArray *json_array;
	gchar **groups;
	gsize ii, n_groups;

	g_return_if_fail (JSON_NODE_HOLDS_ARRAY (json_node));

	json_array = json_node_get_array (json_node);

	json_array_foreach_element (
		json_array,
		fcmdr_gsettings_backend_foreach_element_cb,
		backend->priv->key_file);

	groups = g_key_file_get_groups (backend->priv->key_file, &n_groups);

	for (ii = 0; ii < n_groups; ii++) {
		gchar **keys;
		gsize jj, n_keys;

		keys = g_key_file_get_keys (
			backend->priv->key_file,
			groups[ii], &n_keys, NULL);

		for (jj = 0; jj < n_keys; jj++) {
			g_hash_table_add (
				backend->priv->locks,
				g_strdup_printf (
				"/%s/%s", groups[ii], keys[jj]));
		}

		g_strfreev (keys);
	}

	g_strfreev (groups);
}

static void
fcmdr_gsettings_backend_finalize (GObject *object)
{
	FCmdrGSettingsBackendPrivate *priv;

	priv = FCMDR_GSETTINGS_BACKEND_GET_PRIVATE (object);

	g_key_file_free (priv->key_file);
	g_hash_table_destroy (priv->locks);

	g_free (priv->sysdb_name);
	g_free (priv->sysdb_path);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (fcmdr_gsettings_backend_parent_class)->
		finalize (object);
}

static void
fcmdr_gsettings_backend_constructed (GObject *object)
{
	FCmdrGSettingsBackend *backend;
	FCmdrProfile *profile;
	JsonNode *settings;
	const gchar *uid;

	/* Chain up to parent's constructed() method. */
	G_OBJECT_CLASS (fcmdr_gsettings_backend_parent_class)->
		constructed (object);

	backend = FCMDR_GSETTINGS_BACKEND (object);

	settings = fcmdr_settings_backend_get_settings (
		FCMDR_SETTINGS_BACKEND (backend));

	fcmdr_gsettings_backend_parse_json_node (backend, settings);

	profile = fcmdr_settings_backend_ref_profile (
		FCMDR_SETTINGS_BACKEND (backend));

	uid = fcmdr_profile_get_uid (profile);

	backend->priv->sysdb_name =
		g_strdup_printf ("fleet-commander-%s", uid);

	backend->priv->sysdb_path = g_strdup_printf (
		"/etc/dconf/db/%s.d", backend->priv->sysdb_name);

	g_clear_object (&profile);
}

static void
fcmdr_gsettings_backend_apply_settings (FCmdrSettingsBackend *backend)
{
	FCmdrGSettingsBackend *gsettings_backend;
	gchar *filename = NULL;
	gchar *locks_path;
	GError *local_error = NULL;

	gsettings_backend = FCMDR_GSETTINGS_BACKEND (backend);

	locks_path = g_build_filename (
		gsettings_backend->priv->sysdb_path, "locks", NULL);

	if (g_mkdir_with_parents (locks_path, 0755) == -1) {
		g_critical (
			"Failed to make directory: %s: %s",
			locks_path, g_strerror (errno));
		goto exit;
	}

	filename = g_build_filename (
		gsettings_backend->priv->sysdb_path, "generated", NULL);

	fcmdr_gsettings_backend_write_key_file (
		gsettings_backend, filename, &local_error);

	if (local_error != NULL) {
		g_critical (
			"Failed to write file: %s: %s",
			filename, local_error->message);
		g_clear_error (&local_error);
		goto exit;
	}

	g_free (filename);

	filename = g_build_filename (locks_path, "generated", NULL);

	fcmdr_gsettings_backend_write_locks (
		gsettings_backend, filename, &local_error);

	if (local_error != NULL) {
		g_critical (
			"Failed to write file: %s: %s",
			filename, local_error->message);
		g_clear_error (&local_error);
		goto exit;
	}

exit:
	g_free (filename);
	g_free (locks_path);
}

static void
fcmdr_gsettings_backend_class_init (FCmdrGSettingsBackendClass *class)
{
	GObjectClass *object_class;
	FCmdrSettingsBackendClass *backend_class;

	g_type_class_add_private (
		class, sizeof (FCmdrGSettingsBackendPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->finalize = fcmdr_gsettings_backend_finalize;
	object_class->constructed = fcmdr_gsettings_backend_constructed;

	backend_class = FCMDR_SETTINGS_BACKEND_CLASS (class);
	backend_class->apply_settings = fcmdr_gsettings_backend_apply_settings;
}

static void
fcmdr_gsettings_backend_init (FCmdrGSettingsBackend *backend)
{
	backend->priv = FCMDR_GSETTINGS_BACKEND_GET_PRIVATE (backend);

	backend->priv->key_file = g_key_file_new ();

	backend->priv->locks = g_hash_table_new_full (
		(GHashFunc) g_str_hash,
		(GEqualFunc) g_str_equal,
		(GDestroyNotify) g_free,
		(GDestroyNotify) NULL);
}

