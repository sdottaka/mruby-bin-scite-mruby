MRuby::Build.new do |conf|
  # load specific toolchain settings

  # Gets set by the VS command prompts.
  if ENV['VisualStudioVersion'] || ENV['VSINSTALLDIR']
    toolchain :visualcpp
  else
    toolchain :gcc
  end

  enable_debug

  # include the default GEMs
  conf.gembox 'default'
  conf.gem "#{MRUBY_ROOT}/mrbgems/mruby-eval"
  conf.gem "#{MRUBY_ROOT}/build/mrbgems/mruby-bin-scite-mruby"
  conf.gem :git => 'https://github.com/iij/mruby-io.git'
  conf.gem :git => 'https://github.com/iij/mruby-dir.git'
  conf.gem :git => 'https://github.com/iij/mruby-env.git'
  conf.gem :git => 'https://github.com/iij/mruby-regexp-pcre.git'
  conf.gem :git => 'https://github.com/iij/mruby-pack.git'
  conf.gem :git => 'https://github.com/sdottaka/mruby-win32ole.git'
  conf.gem :git => 'https://github.com/mattn/mruby-require.git'
end

MRuby::Build.new('host-debug') do |conf|
  # load specific toolchain settings

  # Gets set by the VS command prompts.
  if ENV['VisualStudioVersion'] || ENV['VSINSTALLDIR']
    toolchain :visualcpp
  else
    toolchain :gcc
  end

  enable_debug

  # include the default GEMs
  conf.gembox 'default'

  # C compiler settings
  conf.cc.defines = %w(ENABLE_DEBUG)

  # Generate mruby debugger command (require mruby-eval)
  conf.gem :core => "mruby-bin-debugger"

  conf.gem "#{MRUBY_ROOT}/mrbgems/mruby-eval"
  conf.gem "#{MRUBY_ROOT}/build/mrbgems/mruby-bin-scite-mruby"
  conf.gem :git => 'https://github.com/iij/mruby-io.git'
  conf.gem :git => 'https://github.com/iij/mruby-dir.git'
  conf.gem :git => 'https://github.com/iij/mruby-env.git'
  conf.gem :git => 'https://github.com/iij/mruby-regexp-pcre.git'
  conf.gem :git => 'https://github.com/iij/mruby-pack.git'
  conf.gem :git => 'https://github.com/sdottaka/mruby-win32ole.git'
  conf.gem :git => 'https://github.com/mattn/mruby-require.git'
end

