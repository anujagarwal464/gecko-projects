function run_test()
{
  do_get_profile();

  // Open for write, write
  asyncOpenCacheEntry("http://200/", "disk", Ci.nsICacheStorage.OPEN_NORMALLY, null,
    new OpenCallback(NEW, "21m", "21d", function(entry) {
      // Open normally but wait for validation from the server
      asyncOpenCacheEntry("http://200/", "disk", Ci.nsICacheStorage.OPEN_NORMALLY, null,
        new OpenCallback(REVAL, "21m", "21d", function(entry) {
          // emulate 200 from server (new content)
          do_execute_soon(function() {
            var entry2 = entry.recreate();

            // now fill the new entry, use OpenCallback directly for it
            var callback2 = new OpenCallback(NEW, "22m", "22d", function() {});
            callback2.onCacheEntryAvailable(entry2, true, null, Cr.NS_OK);
          });
        })
      );

      var mc = new MultipleCallbacks(3, finish_cache2_test);

      asyncOpenCacheEntry("http://200/", "disk", Ci.nsICacheStorage.OPEN_NORMALLY, null,
        new OpenCallback(NORMAL, "22m", "22d", function(entry) {
          mc.fired();
        })
      );
      asyncOpenCacheEntry("http://200/", "disk", Ci.nsICacheStorage.OPEN_NORMALLY, null,
        new OpenCallback(NORMAL, "22m", "22d", function(entry) {
          mc.fired();
        })
      );
      asyncOpenCacheEntry("http://200/", "disk", Ci.nsICacheStorage.OPEN_NORMALLY, null,
        new OpenCallback(NORMAL, "22m", "22d", function(entry) {
          mc.fired();
        })
      );
    })
  );

  do_test_pending();
}
