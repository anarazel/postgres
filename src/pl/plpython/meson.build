if not python3_dep.found()
  subdir_done()
endif

plpython_sources = files(
  'plpy_cursorobject.c',
  'plpy_elog.c',
  'plpy_exec.c',
  'plpy_main.c',
  'plpy_planobject.c',
  'plpy_plpymodule.c',
  'plpy_procedure.c',
  'plpy_resultobject.c',
  'plpy_spi.c',
  'plpy_subxactobject.c',
  'plpy_typeio.c',
  'plpy_util.c',
)

plpython_sources += custom_target('spiexceptions.h',
    input: files('../../backend/utils/errcodes.txt'),
    output: 'spiexceptions.h',
    command: [perl, files('generate-spiexceptions.pl'), '@INPUT@'],
    capture: true
  )


# FIXME: need to duplicate import library ugliness?
plpython_inc = include_directories('.')

shared_module('plpython3',
  plpython_sources,
  c_pch: '../../include/pch/postgres_pch.h',
  include_directories: [plpython_inc, postgres_inc],
  kwargs: pg_mod_args + {
    'dependencies': [python3_dep, pg_mod_args['dependencies']],
  },
)

# FIXME: Only install the relevant versions
install_data(
  'plpython3u.control',
  'plpython3u--1.0.sql',
  install_dir: get_option('datadir') / 'extension'
)

plpython_regress = [
  'plpython_schema',
  'plpython_populate',
  'plpython_test',
  'plpython_do',
  'plpython_global',
  'plpython_import',
  'plpython_spi',
  'plpython_newline',
  'plpython_void',
  'plpython_call',
  'plpython_params',
  'plpython_setof',
  'plpython_record',
  'plpython_trigger',
  'plpython_types',
  'plpython_error',
  'plpython_ereport',
  'plpython_unicode',
  'plpython_quote',
  'plpython_composite',
  'plpython_subtransaction',
  'plpython_transaction',
  'plpython_drop',
]

regress_tests += {
  'name': 'plpython',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': plpython_regress,
}
