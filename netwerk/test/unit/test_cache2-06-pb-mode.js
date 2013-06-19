function exitPB()
{
  var obsvc = Cc["@mozilla.org/observer-service;1"].
    getService(Ci.nsIObserverService);
  obsvc.notifyObservers(null, "last-pb-context-exited", null);
}

function run_test()
{
  do_get_profile();

  // Store PB entry
  asyncOpenCacheEntry("http://p1/", "disk", Ci.nsICacheStorage.OPEN_NORMALLY, new LoadContextInfo(PRIVATE),
    new OpenCallback(NEW, "p1m", "p1d", function(entry) {
      // Check it's there
      var storage = getCacheStorage("disk", new LoadContextInfo(PRIVATE));
      storage.asyncVisitStorage(
        new VisitCallback(1, 10, ["http://p1/"], function() {
          // Simulate PB exit
          exitPB();
          // Check the entry is gone
          storage.asyncVisitStorage(
            new VisitCallback(0, 0, null, function() {
              finish_cache2_test();
            }),
          true);
        }),
      true);
    })
  );

  do_test_pending();
}
