// Remove Sync-KGraph auxiliary objects for every model. This intentionally
// leaves the application graph untouched.

MATCH (n)
WHERE n:SyncModel OR n:SyncState OR n:SyncLetter OR n:SyncPair OR n:SyncSubset
DETACH DELETE n;

DROP TRIGGER sync_kgraph_mark_dirty;
