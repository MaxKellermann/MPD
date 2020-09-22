icu_dep = dependency('icu-i18n', version: '>= 50', required: get_option('icu'))
conf.set('HAVE_ICU', icu_dep.found())

icu_sources = [
  'CaseFold.cxx',
  'Compare.cxx',
  'Collate.cxx',
  'Converter.cxx',
]

if is_windows
  icu_sources += 'Win32.cxx'
endif

if icu_dep.found()
  icu_sources += [
    'Util.cxx',
    'Init.cxx',
  ]
elif not get_option('iconv').disabled()
  have_iconv = c_compiler.has_header_symbol('iconv.h', 'iconv')
  if have_iconv
    libiconv = c_compiler.find_library('iconv', required : false)
    conf.set('HAVE_ICONV', have_iconv)
    libiconv_dep = static_library('iconv', icu_sources, include_directories: inc, dependencies: libiconv)
    icu_dep = declare_dependency(link_with: libiconv_dep, dependencies: util_dep)
  endif
  if not have_iconv and c_compiler.has_function('iconv')
    libiconv = []
    have_iconv = true
    conf.set('HAVE_ICONV', have_iconv)
  endif
  if not have_iconv and get_option('iconv').enabled()
    error('iconv() not available')
  endif
endif

icu = static_library(
  'icu',
  icu_sources,
  include_directories: inc,
  dependencies: [
    icu_dep,
  ],
)

icu_dep = declare_dependency(
  link_with: icu,
  dependencies: [
    util_dep,
  ],
)
