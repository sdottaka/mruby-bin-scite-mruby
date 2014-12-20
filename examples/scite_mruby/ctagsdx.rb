# based on ctagsdx.lua
# browse a tags database from SciTE!
# Set this property:
# ctags.path.cxx=<full path to tags file>
# 1. Multiple tags are handled correctly; a drop-down
# list is presented
# 2. There is a full stack of marks available.
# 3. If ctags.path.cxx is not defined, will try to find a tags file in the current dir.

SciTE.command [
  'Find Tag|find_ctag $(CurrentWord)|Ctrl+.',
  'Go to Mark|goto_mark|Alt+.',
  'Set Mark|set_mark|Ctrl+\'',
  'Select from Mark|select_mark|Ctrl+/',
]

$gMarkStack = []

# this centers the cursor position
# easy enough to make it optional!
def ctags_center_pos(line = nil)
  if !line then
    line = Editor.LineFromPosition(Editor.CurrentPos)
  end
  top = Editor.FirstVisibleLine
  middle = top + Editor.LinesOnScreen/2
  Editor.LineScroll(0,line - middle)
end

def open_file(file,line,was_pos)
  SciTE.open(file)
  if !was_pos then
    Editor.GotoLine(line)
    ctags_center_pos(line)
  else
    Editor.GotoPos(line)
    ctags_center_pos()
  end
end

def set_mark()
  $gMarkStack.push({ :file => Props['FilePath'], :pos => Editor.CurrentPos })
end

def goto_mark()
  mark = $gMarkStack.pop
  if mark then
    open_file(mark[:file],mark[:pos],true)
  end
end

def select_mark()
  mark = $gMarkStack.last
  if mark then
    p1 = mark[:pos]
    p2 = Editor.CurrentPos
    Editor.SetSel(p1,p2)
  end
end

def ReadTagFile(file)
  if !File.exist?(file) then return [] end

  tags = {}
  # now we can pick up the tags!
  File.open(file) do |f|
    re = /^([^\t]+)\t/
    while line = f.gets
      # skip if line is comment
      if line[0] != '!' && (m = line.match(re))
        tag = m[1]
        unless tags.include?(tag) then
          tags[tag] = line + '@'
        else
          tags[tag] += line + '@'
        end
      end
    end
  end
  return tags
end

$gTagFile = ""
$tags = {}

def OpenTag(tag)
  # ask SciTE to open the file
  file_name = tag[:file]
  path = File.dirname($gTagFile)
  if path then file_name = File.join(path, file_name) end
  set_mark()
  SciTE.open(file_name)
  # depending on what kind of tag, either search for the pattern,
  # or go to the line.
  pattern = tag[:pattern]
  if pattern.kind_of?(String) then
    p1, _ = Editor.findtext(pattern)
    if p1 then
      Editor.GotoPos(p1)
      ctags_center_pos()
    end
  else
    tag_line = pattern
    Editor.GotoLine(tag_line)
    ctags_center_pos(tag_line)
  end
end

def locate_tags(dir)
  filefound = nil
  while dir do
    file = File.join(dir, "tags")
    #puts "---" + file
    if File.exist?(file) then
      filefound = file
      break
    end
    dir = dir.chomp("/").chomp("\\")
    parent = File.dirname(dir)
    break if parent == dir
    dir = parent
  end
  return filefound
end

def find_ctag(f,partial = true)
  # search for tags files first
  result = Props['ctags.path.cxx']
  if result.to_s == "" then
    result = locate_tags(Props['FileDir'])
  end
  if !result then
    puts "No tags found!"
    return
  end
  if result != $gTagFile then
    puts "Reloading tag from:" + result
    $gTagFile = result
    $tags = ReadTagFile($gTagFile)
  end
  if partial then
    result = ''
    $tags.each do |tag, val|
      if tag.index(f) then
        result = result + val + '@'
      end
    end
  else
    result = $tags[f]
  end
  if !result then return end  # not found
  matches = []
  k = 0;
  for line in result.split("@") do
    # split this into the three tab-separated fields
    # _extended_ ctags format ends in ;"
    next if line == ""
    _,tag_name,file_name,tag_pattern = line.match(/([^\t]*)\t([^\t]*)\t(.*)/).to_a
    # for Exuberant Ctags
    s3 = tag_pattern.match(/(.*);\"/)[1]
    if s3 then
      tag_pattern = s3
    end
    s1 = tag_pattern.match(/(.*)\$\/$/)
    if s1 then
      tag_pattern = s1[1][2..-1]
      tag_pattern = tag_pattern.gsub('\\/','/')
      matches[k] = {:tag => f, :file => file_name, :pattern => tag_pattern}
    else
      tag_line = tag_pattern.to_i-1
      matches[k] = {:tag => f, :file => file_name, :pattern => tag_line}
    end
    k = k + 1
  end

  if k == 0 then return end
  if k > 1 then # multiple tags found
    list = []
    matches.each_with_index do |t, i|
      list.push("#{i + 1} #{t[:file]}:#{t[:pattern]}")
    end
    SciTE.userListShow list do |s|
      tok = s.match(/^(\d+)/)[1]
      idx = tok.to_i - 1 # very important!
      OpenTag(matches[idx])
    end
  else
    OpenTag(matches[0])
  end
end

