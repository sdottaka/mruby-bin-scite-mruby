# based on switch_headers.lua
# toggles between C++ source files and corresponding header files
SciTE.define_command('Switch Source/Header', '*.c;*.cpp;*.cxx;*.c++;*.h;*.hpp', 'Shift+Ctrl+H') do
  cpp_exts = ['cpp','cxx','c++','c']
  hpp_exts = ['h','hpp']

  def does_exist(basename,extensions)
    extensions.each do |ext|
      f = basename + '.' + ext
      if File.exist?(f) then return f end
    end
    return nil
  end

  file = Props['FilePath']
  ext = Props['FileExt']
  basename = Props['FileDir'] + '/' + Props['FileName']
  if cpp_exts.include?(ext) then
    other = does_exist(basename,hpp_exts)
  elsif hpp_exts.include?(ext) then
    other = does_exist(basename,cpp_exts)
  else
    puts('not a C++ file: ' + file); return
  end
  if !other then 
    puts('source/header does not exist: ' + file)
  else
    SciTE.open(other)
  end
end
