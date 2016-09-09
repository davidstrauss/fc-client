namespace FleetCommander {
  public class ConfigurationAdapterNM : ConfigurationAdapter, Object {
    NetworkManager.SettingsHelper nmsh;

    public ConfigurationAdapterNM (NetworkManager.SettingsHelper nmsh) {
      this.nmsh = nmsh;
      this.nmsh.bus_appeared.connect (() => { available (this); });
    }

    public void bootstrap (UserIndex index, CacheData profiles_cache, Logind.User[] users) {
      debug ("Bootstrapping NetworkManager settings for all logged in users");
      if (nmsh.in_bus () == false) {
        debug ("NetworkManager's bus name was not connected");
        return;
      }

      foreach (var user in users) {
        update (index, profiles_cache, user.uid);
      }
    }

    public void update (UserIndex index, CacheData profiles_cache, uint32 uid) {
      debug ("Updating NM connections for user %u", uid);
      if (nmsh.in_bus () == false) {
        debug ("NetworkManager's bus name was not connected");
        return;
      }

      var name = Posix.uid_to_user_name (uid);
      var groups = Posix.get_groups_for_user (uid);

      var profile_uids = index.get_profiles_for_user_and_groups (name, groups);

      if (profile_uids.length < 1) {
        debug ("User %u did not have any profiles assigned", uid);
        return;
      }

      var profiles = profiles_cache.get_profiles (profile_uids);

      if (profiles == null || profiles.length != profile_uids.length) {
        string uids = "";
        foreach (var u in profile_uids) {
          uids = "%s, %s".printf (uids, u);
        }

        if (profiles == null) {
          debug ("No profiles found from list: %s", uids);
          return;
        }

        debug ("Some profiles applied to user %u were not found", uid);
      }

      foreach (var profile in profiles) {
        var gen = new Json.Generator ();
        var node = new Json.Node (Json.NodeType.OBJECT);
        node.set_object (profile);
        gen.set_root (node);

        var settings = profile.get_object_member("settings");
        var puuid = profile.get_string_member ("uid");
        if (settings == null)
          continue;

        var nm = settings.get_array_member ("org.freedesktop.NetworkManager");
        if (nm == null)
          continue;

        nm.foreach_element ((a, i, connection_node) => {
          var root = connection_node.get_object ();
          string uuid;

          try {
            uuid = Json.Path.query ("$.uuid", connection_node).get_array ().get_string_element (0);
            if (uuid == null)
              throw new FCError.ERROR ("Could not find uuid member in profile %s, skipping".printf (puuid));
          } catch (GLib.Error e) {
            warning ("NetworkManager connection %u from profile %s does not have a uuid in the form of a string",
                     i, puuid);
            return;
          }

          debug ("Adding network profile %s to user %s", uuid, name);

          var serialized_data = root.get_string_member ("data");
          if (serialized_data == null) {
            warning ("NetworkManager connection %u from profile %s does not have a 'data' member",
                     i, puuid);
            return;
          }

          Variant conn_variant;
          try {
            conn_variant = Variant.parse (null, serialized_data, null, null);
          } catch (Error e) {
            warning ("Could not parse 'data' member of %u connection in profile %s into a variant:\n%s\n%s",
                     i, puuid, e.message, serialized_data);
            return;
          }

          //FIXME: Add permissions and other modifications here

          var nm_conn_path = nmsh.get_connection_path_by_uid (uuid);
          try {
            if (nm_conn_path == null) {
              debug ("Connection %s didn't exist already, adding", uuid);
              nmsh.add_connection (conn_variant);
            } else {
              debug ("Connection %s already existed, updating", uuid);
              nmsh.update_connection (nm_conn_path, conn_variant);
            }
          } catch (Error e) {
            warning ("Could not add/update %u connection from profile %s, there was an error making the DBus call:\n%s",
                     i, puuid, e.message);
          }
        });
      }
    }
  }
}