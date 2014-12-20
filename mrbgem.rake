# require 'mkmf'

MRuby::Gem::Specification.new('mruby-bin-scite') do |spec|
  spec.license = ['Historical Permission Notice and Disclaimer (SciTE)', 'MIT License (ExtMan)']
  spec.authors = ['Takashi Sawanaka']
  spec.description = "SciTE-based text editor with mruby scripting extension"
  spec.homepage = 'https://github.com/sdottaka/mruby-bin-scite-mruby'
  
#  spec.bins = %w(scite)

  spec.add_dependency 'mruby-print'

  exepath = exefile("#{build.build_dir}/bin/scite")
  installpath = exefile("#{MRUBY_ROOT}/bin/scite")
  unless ENV['OS'] == 'Windows_NT'
    gtkversion = "gtk+-2.0"
    prefix =`pkg-config --variable=prefix "#{gtkversion}" 2>/dev/null`.chomp
    datadir = "#{prefix}/share"
    pixmapdir = "#{datadir}/pixmaps"
    sysconf_path = "#{prefix}/share/scite"
    gtk_flags = `pkg-config --cflags "#{gtkversion}"`.chomp
  end

  task :all => exepath

  if build.name == "host"
    task :all => installpath
    file installpath => exepath do |t|
      FileUtils.rm_f t.name, { :verbose => $verbose }
      FileUtils.cp t.prerequisites.first, t.name, { :verbose => $verbose }
    end
  end
  
  spec.cxx.include_paths += ["#{dir}/tools/scintilla/include", "#{dir}/tools/scintilla/lexlib", "#{dir}/tools/scintilla/src"]
  spec.cxx.include_paths += ["#{dir}/tools/scite/lua/include", "#{dir}/tools/scite/src"]
  spec.cc.include_paths  += ["#{dir}/tools/scite/lua/include", "#{dir}/tools/scite/src"]
  if ENV['OS'] == 'Windows_NT'
    spec.cxx.flags += ["-D_UNICODE", "-DUNICODE", "-DSTATIC_BUILD", "-DSCI_LEXER"]
    spec.cc.flags << '-DLUA_USER_H=\"scite_lua_win.h\"'
  else
    spec.cxx.flags += [gtk_flags, "-DSCI_LEXER", "-DGTK", "-DPIXMAP_PATH=\\\"#{pixmapdir}\\\"", "-DSYSCONF_PATH=\\\"#{sysconf_path}\\\""]
    spec.cc.flags << "-DLUA_USE_POSIX" << gtk_flags
    spec.cxx.flags << "-Dunix" if `uname -s` =~ /Darwin/
  end

  srcs  = Dir.glob([
           "#{dir}/tools/scintilla/src/*.cxx",
           "#{dir}/tools/scintilla/lexlib/*.cxx",
           "#{dir}/tools/scintilla/lexers/*.cxx",
           "#{dir}/tools/scite/src/*.cxx",
           "#{dir}/tools/scite/lua/src/*.c",
           "#{dir}/tools/scite/lua/src/lib/*.c"
          ])

  if ENV['OS'] == 'Windows_NT'
    srcs += Dir.glob(["#{dir}/tools/scintilla/win32/*.cxx", "#{dir}/tools/scite/win32/*.cxx"])
  else
    srcs += Dir.glob(["#{dir}/tools/scintilla/gtk/*.c*", "#{dir}/tools/scite/gtk/*.cxx"])
  end

  objs = srcs.map { |f| f.relative_path_from(dir).pathmap("#{build_dir}/%X#{spec.exts.object}" ) }

  if ENV['OS'] == 'Windows_NT'
    # Check if Direct2D headers are available by trying to compile a file that includes them.
    # Most distributions of MinGW32 do not include Direct2D support but MinGW64 does.
    if !system("#{spec.cxx.command} -c #{dir}/tools/scintilla/win32/CheckD2D.cxx > NUL 2>&1")
      spec.cxx.flags << "-DDISABLE_D2D"
      objs.reject! {|f| f =~ /CheckD2D/ }
    end
 
    if spec.cc.command =~ /cl(\.exe)?$/
      objs << "#{build_dir}/tools/scite/win32/SciTERes.res"
      file "#{build_dir}/tools/scite/win32/SciTERes.res" => "#{dir}/tools/scite/win32/SciTERes.rc" do |t|
        sh "rc -DSTATIC_BUILD -I#{dir}/tools/scite/src -fo #{t.name} #{t.prerequisites[0]}"
      end
    else
      objs << "#{build_dir}/tools/scite/win32/SciTERes.o"
      file "#{build_dir}/tools/scite/win32/SciTERes.o" => "#{dir}/tools/scite/win32/SciTERes.rc" do |t|
        sh "windres -DSTATIC_BUILD -I#{dir}/tools/scite/src -i #{t.prerequisites[0]} -o #{t.name}"
      end
    end
  end

  file exefile("#{build.build_dir}/bin/scite") => objs + [libfile("#{build.build_dir}/lib/libmruby")]  do |t|
    current_target = build
    MRuby.each_target do |target|
     if target == current_target
        gem_flags = gems.map { |g| g.linker.flags }
        gem_flags_before_libraries = gems.map { |g| g.linker.flags_before_libraries }
        gem_flags_after_libraries = gems.map { |g| g.linker.flags_after_libraries }
        gem_libraries = gems.map { |g| g.linker.libraries }
        gem_library_paths = gems.map { |g| g.linker.library_paths }

        if ENV['OS'] == 'Windows_NT'
          gem_libraries += %w(ole32 oleaut32 advapi32 user32 gdi32 imm32 msimg32 comdlg32 comctl32 shell32 uuid uxtheme)
        else
          gem_libraries << 'm'
          gem_libraries << 'dl' if `uname -s` =~ /Linux|GNU/
          gem_flags_after_libraries << `pkg-config --libs #{gtkversion} gthread-2.0 gmodule-no-export-2.0`.chomp
        end
        gem_libraries << 'stdc++' unless spec.cc.command =~ /cl(\.exe)?$/

        linker.run t.name, t.prerequisites, gem_libraries, gem_library_paths, gem_flags, gem_flags_before_libraries, gem_flags_after_libraries
      end
    end
  end

=begin
  target = exefile("#{MRUBY_ROOT}/bin/scite")
  temp_exe = (ENV['OS'] == 'Windows_NT') ? "#{dir}/tools/scite/bin/Sc1" : "#{dir}/tools/scite/bin/SciTE"

  task :all => target

  file target => temp_exe do |t|
    while File.exist?(target)
      begin FileUtils.rm target; rescue; puts "retrying to remove #{target} ..."; sleep 1; end
    end
    FileUtils.cp temp_exe, target
  end
  
  file temp_exe => "#{build.build_dir}/lib/libmruby.flags.mak" do |t|
    exec_make spec
  end

  task :clean do
    exec_make spec, "clean"
  end

  def exec_make(spec, target = "")
    replace_str_file "#{build.build_dir}/lib/libmruby.flags.mak", " mruby.lib", " libmruby.lib"

    if ENV['OS'] == 'Windows_NT'
      if spec.cc.command =~ /gcc/
        make = 'make'
        make = 'mingw32-make' if find_executable('mingw32-make')
        sh "#{make} -C #{dir}/tools/scintilla/win32 #{target}"
        sh "#{make} -C #{dir}/tools/scite/win32     #{target}"
      elsif spec.cc.command =~ /cl(\.exe)?$/
        nmake_macro = spec.cc.flags.to_s =~ /[-\/]MD/ ? "USE_MSVCRT=" : ""
        sh "cd #{dir}/tools/scintilla/win32 & nmake -f scintilla.mak #{nmake_macro} #{target}"
        sh "cd #{dir}/tools/scite/win32     & nmake -f scite.mak     #{nmake_macro} #{target}"
      end
    else
      sh "make -C #{dir}/tools/scintilla/gtk #{target}"
      sh "make -C #{dir}/tools/scite/gtk     #{target}"
    end
  end

  def replace_str_file(file, pattern, replace)
    lines = ""
    if File.exist?(file)
      File.open(file, "r") do |i|
        lines = (i.read).gsub(pattern, replace)
      end
    end
    File.open(file, "w") { |o| o.write lines }
  end
=end
end
