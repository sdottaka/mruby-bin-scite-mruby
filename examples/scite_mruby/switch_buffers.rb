# based on switch_buffers.lua
# drops down a list of buffers, in recently-used order

SciTE.command 'Switch Buffer|do_buffer_list|Alt+F12'
SciTE.command 'Last Buffer|last_buffer|Ctrl+F12'

$scite_buffers = []

SciTE.onOpenSwitch do |f|
  # swop the new current buffer with the last one!
  $scite_buffers.delete f
  $scite_buffers.unshift f
  $scite_buffers.delete ""
  false
end

def last_buffer()
  if $scite_buffers.length > 1
    SciTE.open($scite_buffers[1])
  end
end

def do_buffer_list()
  SciTE.userListShow $scite_buffers, 1 do |f|
    SciTE.open f
  end
end
