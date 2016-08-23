namespace FleetCommander {
  public interface ConfigurationAdapter : Object {
    public signal void available (ConfigurationAdapter adapter);
    public abstract void bootstrap (UserIndex index, CacheData profiles_cache, Logind.User[] users);
    public abstract void update (UserIndex index, CacheData profiles_cache, uint32 uid);
  }
}
