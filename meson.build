# Copyright (c) 2025, Postgres Professional

# Does not support the PGXS infrastructure at this time. Please, compile as part
# of the contrib source tree.

ptrack_sources = files(
  'datapagemap.c',
  'engine.c',
  'ptrack.c',
)

if host_system == 'windows'
  ptrack_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'ptrack',
    '--FILEDESC', 'ptrack - block-level incremental backup engine for Postgres Pro.',])
endif

ptrack = shared_module('ptrack',
  ptrack_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += ptrack

install_data(
  'ptrack.control',
  'ptrack--2.0--2.1.sql',
  'ptrack--2.1--2.2.sql',
  'ptrack--2.1.sql',
  'ptrack--2.2--2.3.sql',
  'ptrack--2.3--2.4.sql',
  'ptrack--2.4--2.5.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'ptrack',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_basic.pl',
      't/002_cfs_compatibility.pl',
    ],
  },
}
