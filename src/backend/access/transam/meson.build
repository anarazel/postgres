backend_sources += files(
  'clog.c',
  'commit_ts.c',
  'generic_xlog.c',
  'multixact.c',
  'parallel.c',
  'rmgr.c',
  'slru.c',
  'subtrans.c',
  'timeline.c',
  'transam.c',
  'twophase.c',
  'twophase_rmgr.c',
  'varsup.c',
  'xact.c',
  'xlog.c',
  'xlogarchive.c',
  'xlogfuncs.c',
  'xloginsert.c',
  'xlogrecovery.c',
  'xlogutils.c',
)

# used by frontend programs to built a frontend xlogreader
xlogreader_sources = files(
  'xlogreader.c',
)

backend_sources += xlogreader_sources
