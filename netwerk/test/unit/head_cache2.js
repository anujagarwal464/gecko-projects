const Cc = Components.classes;
const Ci = Components.interfaces;
const Cu = Components.utils;
const Cr = Components.results;

var callbacks = new Array();

// Expect an existing entry
const NORMAL =               0;
// Expect a new entry
const NEW =             1 << 0;
// Return ENTRY_NOT_VALID from onCacheEntryCheck
const NOTVALID =        1 << 1;
// Throw from onCacheEntryAvailable
const THROWAVAIL =      1 << 2;
// Open entry for reading-only
const READONLY =        1 << 3;
// Expect the entry to not be found
const NOTFOUND =        1 << 4;
// Return ENTRY_NEEDS_REVALIDATION from onCacheEntryCheck
const REVAL =           1 << 5;
// Return ENTRY_PARTIAL from onCacheEntryCheck, in combo with NEW or RECREATE bypasses check for emptiness of the entry
const PARTIAL =         1 << 6
// Expect the entry is doomed, i.e. the output stream should not be possible to open
const DOOMED =          1 << 7;
// Expect the entry is doomed, i.e. the output stream should not be possible to open
const WAITFORWRITE =    1 << 8;
// Don't write data (i.e. don't open output stream)
const METAONLY =        1 << 9;
// Do recreation of an existing cache entry
const RECREATE =        1 << 10;

var log_c2 = true;
function LOG_C2(o, m)
{
  if (!log_c2) return;
  if (!m)
    dump("TEST-INFO | CACHE2: " + o + "\n");
  else
    dump("TEST-INFO | CACHE2: callback #" + o.order + "(" + (o.workingData || "---") + ") " + m + "\n");
}

function pumpReadStream(inputStream, goon)
{
  var pump = Cc["@mozilla.org/network/input-stream-pump;1"]
             .createInstance(Ci.nsIInputStreamPump);
  pump.init(inputStream, -1, -1, 0, 0, true);
  var data = "";
  pump.asyncRead({
    onStartRequest: function (aRequest, aContext) { },
    onDataAvailable: function (aRequest, aContext, aInputStream, aOffset, aCount)
    {
      var wrapper = Cc["@mozilla.org/scriptableinputstream;1"].
                    createInstance(Ci.nsIScriptableInputStream);
      wrapper.init(aInputStream);
      var str = wrapper.read(wrapper.available());
      LOG_C2("reading data '" + str + "'");
      data += str;
    },
    onStopRequest: function (aRequest, aContext, aStatusCode)
    {
      LOG_C2("done reading data: " + aStatusCode);
      do_check_eq(aStatusCode, Cr.NS_OK);
      goon(data);
    },
  }, null);
}

OpenCallback.prototype =
{
  QueryInterface: function listener_qi(iid) {
    if (iid.equals(Ci.nsISupports) ||
        iid.equals(Ci.nsICacheEntryOpenCallback)) {
      return this;
    }
    throw Components.results.NS_ERROR_NO_INTERFACE;
  },
  onCacheEntryCheck: function(entry, appCache)
  {
    LOG_C2(this, "onCacheEntryCheck");
    do_check_true(!this.onCheckPassed);
    this.onCheckPassed = true;

    if (this.behavior & NOTVALID) {
      LOG_C2(this, "onCacheEntryCheck DONE, return ENTRY_NOT_VALID");
      return Ci.nsICacheEntryOpenCallback.ENTRY_NOT_VALID;
    }

    do_check_eq(entry.getMetaDataElement("meto"), this.workingMetadata);

    // check for sane flag combination
    do_check_neq(this.behavior & (REVAL|PARTIAL), REVAL|PARTIAL);

    if (this.behavior & (REVAL|PARTIAL)) {
      LOG_C2(this, "onCacheEntryCheck DONE, return REVAL");
      return Ci.nsICacheEntryOpenCallback.ENTRY_NEEDS_REVALIDATION;
    }

    LOG_C2(this, "onCacheEntryCheck DONE, return ENTRY_VALID");
    return Ci.nsICacheEntryOpenCallback.ENTRY_VALID;
  },
  onCacheEntryAvailable: function(entry, isnew, appCache, status)
  {
    LOG_C2(this, "onCacheEntryAvailable, " + this.behavior);
    do_check_true(!this.onAvailPassed);
    this.onAvailPassed = true;

    do_check_eq(isnew, !!(this.behavior & NEW));

    if (this.behavior & NOTFOUND) {
      do_check_eq(status, Cr.NS_ERROR_CACHE_KEY_NOT_FOUND);
      do_check_false(!!entry);
      if (this.behavior & THROWAVAIL)
        this.throwAndNotify(entry);
      this.goon(entry);
    }
    else if (this.behavior & (NEW|RECREATE)) {
      do_check_true(!!entry);

      if (this.behavior & RECREATE) {
        entry = entry.recreate();
        do_check_true(!!entry);
      }

      if (this.behavior & THROWAVAIL)
        this.throwAndNotify(entry);

      if (!(this.behavior & WAITFORWRITE))
        this.goon(entry);

      if (!(this.behavior & PARTIAL)) {
        try {
          entry.getMetaDataElement("meto");
          do_check_true(false);
        }
        catch (ex) {}
      }

      var self = this;
      do_execute_soon(function() { // emulate network latency
        entry.setMetaDataElement("meto", self.workingMetadata);
        entry.metaDataReady();
        if (self.behavior & METAONLY) {
          // Since forcing GC/CC doesn't trigger OnWriteRClosed, we have to set the entry valid manually :(
          entry.setValid();
          return;
        }
        do_execute_soon(function() { // emulate more network latency
          if (self.behavior & DOOMED) {
            try {
              var os = entry.openOutputStream(0);
              do_check_true(false);
            } catch (ex) {
              do_check_true(true);
            }
            if (self.behavior & WAITFORWRITE)
              self.goon(entry);
            return;
          }

          var offset = (self.behavior & PARTIAL)
            ? entry.dataSize
            : 0;
          LOG_C2(self, "openOutputStream @ " + offset);
          var os = entry.openOutputStream(offset);
          LOG_C2(self, "writing data");
          var wrt = os.write(self.workingData, self.workingData.length);
          do_check_eq(wrt, self.workingData.length);
          os.close();
          if (self.behavior & WAITFORWRITE)
            self.goon(entry);
        })
      })
    }
    else { // NORMAL
      do_check_true(!!entry);
      do_check_eq(entry.getMetaDataElement("meto"), this.workingMetadata);
      if (this.behavior & THROWAVAIL)
        this.throwAndNotify(entry);

      var wrapper = Cc["@mozilla.org/scriptableinputstream;1"].
                    createInstance(Ci.nsIScriptableInputStream);
      var self = this;
      pumpReadStream(entry.openInputStream(0), function(data) {
        do_check_eq(data, self.workingData);
        self.onDataCheckPassed = true;
        LOG_C2(self, "entry read done");
        self.goon(entry);
      });
    }
  },
  get mainThreadOnly() {
    return true;
  },
  selfCheck: function()
  {
    LOG_C2(this, "selfCheck");

    do_check_true(this.onCheckPassed);
    do_check_true(this.onAvailPassed);
    do_check_true(this.onDataCheckPassed);
  },
  throwAndNotify: function(entry)
  {
    LOG_C2(this, "Throwing");
    var self = this;
    do_execute_soon(function() {
      LOG_C2(self, "Notifying");
      self.goon(entry);
    });
    throw Cr.NS_ERROR_FAILURE;
  }
};

function OpenCallback(behavior, workingMetadata, workingData, goon)
{
  this.behavior = behavior;
  this.workingMetadata = workingMetadata;
  this.workingData = workingData;
  this.goon = goon;
  this.onCheckPassed = (!!(behavior & (NEW|RECREATE)) || !workingMetadata) && !(behavior & NOTVALID);
  this.onAvailPassed = false;
  this.onDataCheckPassed = !!(behavior & (NEW|RECREATE)) || !workingMetadata;
  callbacks.push(this);
  this.order = callbacks.length;
}

VisitCallback.prototype =
{
  QueryInterface: function listener_qi(iid) {
    if (iid.equals(Ci.nsISupports) ||
        iid.equals(Ci.nsICacheStorageVisitor)) {
      return this;
    }
    throw Components.results.NS_ERROR_NO_INTERFACE;
  },
  onCacheStorageInfo: function(num, consumption)
  {
    LOG_C2(this, "onCacheStorageInfo: num=" + num + ", size=" + consumption);
    do_check_eq(this.num, num);
    do_check_eq(this.consumption, consumption);

    if (!this.entries || num == 0)
      this.notify();
  },
  onCacheEntryInfo: function(entry)
  {
    var key = entry.key;
    LOG_C2(this, "onCacheEntryInfo: key=" + key);

    do_check_true(!!this.entries);

    var index = this.entries.indexOf(key);
    do_check_true(index > -1);

    this.entries.splice(index, 1);

    if (this.entries.length == 0) {
      this.entries = null;
      this.notify();
    }
  },
  notify: function()
  {
    do_check_true(!!this.goon);
    var goon = this.goon;
    this.goon = null;
    goon();
  },
  selfCheck: function()
  {
    do_check_true(!this.entries || !this.entries.length);
  }
};

function VisitCallback(num, consumption, entries, goon)
{
  this.num = num;
  this.consumption = consumption;
  this.entries = entries;
  this.goon = goon;
  callbacks.push(this);
  this.order = callbacks.length;
}

EvictionCallback.prototype =
{
  QueryInterface: function listener_qi(iid) {
    if (iid.equals(Ci.nsISupports) ||
        iid.equals(Ci.nsICacheEntryDoomCallback)) {
      return this;
    }
    throw Components.results.NS_ERROR_NO_INTERFACE;
  },
  onCacheEntryDoomed: function(result)
  {
    do_check_eq(this.expectedSuccess, result == Cr.NS_OK);
    this.goon();
  },
  selfCheck: function() {}
}

function EvictionCallback(success, goon)
{
  this.expectedSuccess = success;
  this.goon = goon;
  callbacks.push(this);
  this.order = callbacks.length;
}

MultipleCallbacks.prototype =
{
  fired: function()
  {
    if (--this.pending == 0)
      this.goon();
  }
}

function MultipleCallbacks(number, goon)
{
  this.pending = number;
  this.goon = goon;
}

function finish_cache2_test()
{
  callbacks.forEach(function(callback, index) {
    callback.selfCheck();
  });
  do_test_finished();
}
