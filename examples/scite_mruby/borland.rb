# based on scite_lua/borland.lua
#
# demonstrates how to capture multiple key sequences, like 'ctrl-k 1', with extman.
# This is used to implement Borland-style markers.
SciTE.command [
  'ctrl-k|Borland::do_ctrl_command k|Ctrl+K',
  'ctrl-q|Borland::do_ctrl_command q|Ctrl+Q'
]

module Borland

  @gMarksMap = {}
  @gMarks = {}
  @markers_defined = false
  @base = 9

  SciTE.onOpen do |f|
    @gMarksMap[f] = {}
    false
  end

  SciTE.onSwitchFile do |f|
    @gMarks = @gMarksMap[f]
    false
  end

  def self.current_line
    return Editor.LineFromPosition(Editor.CurrentPos)+1
  end

  def self.define_markers
    zero = "0".bytes[0]
    for i in 1..9 do
        Editor.markerDefine(i+@base,SciTE::SC_MARK_CHARACTER + zero + i)
    end
    @markers_defined = true
  end

  def self.do_ctrl_command(key)
    Editor.beginUndoAction
    SciTE.onChar('once') do |ch|
      Editor.endUndoAction
      Editor.undo
      num = ch.to_i
      mark = num >= 0 && @gMarks[num]
      line = current_line
      if key == 'k' && num
        if !@markers_defined then define_markers end
        if mark # clear mark
           Editor.markerDelete(@gMarks[num]-1,num+@base)
           puts "clear mark line=#{@gMarks[num]}"
           @gMarks[num] = nil
        else
           @gMarks[num] = line
           Editor.markerAdd(line-1,num+@base)
           puts "mark line=#{line}"
        end
      elsif key == 'q' && mark
        Editor.gotoLine(mark-1)
        if respond_to?(:ctags_center_pos) then ctags_center_pos(mark-1) end
      end
      true
    end
  end
end
