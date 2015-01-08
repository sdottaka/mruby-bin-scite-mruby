# based on extman.lua written by Steve Donovan

module SciTE
  EVENT_MARGIN_CLICK = 0
  EVENT_DOUBLE_CLICK = 1
  EVENT_SAVE_POINT_LEFT = 2
  EVENT_SAVE_POINT_REACHED = 3
  EVENT_CHAR = 4
  EVENT_SAVE = 5
  EVENT_BEFORE_SAVE = 6
  EVENT_SWITCH_FILE = 7
  EVENT_OPEN = 8
  EVENT_UPDATE_UI = 9
  EVENT_KEY = 10
  EVENT_DWELL_START = 11
  EVENT_CLOSE = 12
  EVENT_EDITOR_LINE = 13
  EVENT_OUTPUT_LINE = 14
  EVENT_OPEN_SWITCH = 15
  EVENT_USER_LIST_SELECTION = 16
  EVENT_STRIP = 17

  @event_handlers = []
  @menu_idx = 10
  @commands = {}
  @shortcuts_used = {}

  class << self

    def dispatch_one(handlers, *args)
      ret = false
      if handlers
        handlers.each_index do |i|
          ret = handlers[i][:block].call(*args)
          handlers.delete_at(i) if handlers[i][:remove] 
          break if ret
        end
      end
      ret
    end

    def call_command(name, param = nil)
      @commands[name].call param
    end

    def define_command(name, mode = "*", shortcut = nil, param = nil, &block) 
      idx = @menu_idx
      for ii in 10..@menu_idx do
        idx = ii if Props["command.name.#{ii}.#{mode}"] == name
      end
      which = "#{idx}.#{mode}"
      if shortcut
        cmd = @shortcuts_used[shortcut]
        if cmd && cmd != name
          raise "Error: shortcut already used in \"#{cmd}\""
        end
        Props["command.shortcut.#{which}"] = shortcut
        @shortcuts_used[shortcut] = name
      end
      Props["command.name.#{which}"] = name
      Props["command.#{which}"] = "mruby:eval SciTE.call_command '#{name}'" + (param ? ", '#{param}'" : "")
      Props["command.subsystem.#{which}"] = "3"
      Props["command.mode.#{which}"] = "savebefore:no"
      @commands[name] = block
      @menu_idx += 1 if idx == @menu_idx
    end

    def send2(method, arg)
        obj = Object
        names = method.split("::")
        names[0..-2].each {|name| obj = obj.const_get(name) }
        if arg
          obj.__send__ names[-1], arg
        else
          obj.__send__ names[-1]
        end
    end

    def command(tbl)
      if tbl.kind_of?(String)
        tbl = [tbl]
      end
      tbl.each do |v|
        name, cmd, mode, shortcut = v.split("|")
        unless shortcut
          shortcut = mode
          mode = "*"
        end
       	define_command(name, mode, shortcut, cmd.split[1]) {|arg| send2 cmd.split[0], arg }
      end
    end

    def add_event_handler(event, remove = false, &blk)
      @event_handlers[event] ||= []
      @event_handlers[event] << {:block => blk, :remove => remove}
      @event_handlers[event].uniq!
    end

    def on_margin_click(remove = false, &blk)
      add_event_handler EVENT_MARGIN_CLICK, remove, &blk
    end
    def on_double_click(remove = false, &blk)
      add_event_handler EVENT_DOUBLE_CLICK, remove, &blk
    end
    def on_save_point_left(remove = false, &blk)
      add_event_handler EVENT_SAVE_POINT_LEFT, remove, &blk
    end
    def on_save_point_reached(remove = false, &blk)
      add_event_handler EVENT_SAVE_POINT_REACHED, remove, &blk
    end
    def on_open(remove = false, &blk)
      add_event_handler EVENT_OPEN, remove, &blk
    end
    def on_switch_file(remove = false, &blk)
      add_event_handler EVENT_SWITCH_FILE, remove, &blk
    end
    def on_save_before(remove = false, &blk)
      add_event_handler EVENT_SAVE_BEFORE, remove, &blk
    end
    def on_save(remove = false, &blk)
      add_event_handler EVENT_SAVE, remove, &blk
    end
    def on_update_ui(remove = false, &blk)
      add_event_handler EVENT_UPDATE_UI, remove, &blk
    end
    def on_char(remove = false, &blk)
      add_event_handler EVENT_CHAR, remove, &blk
    end
    def on_key(remove = false, &blk)
      add_event_handler EVENT_KEY, remove, &blk
    end
    def on_dwell_start(remove = false, &blk)
      add_event_handler EVENT_DWELL_START, remove, &blk
    end
    def on_close(remove = false, &blk)
      add_event_handler EVENT_CLOSE, remove, &blk
    end
    def on_open_switch(remove = false, &blk)
      add_event_handler EVENT_OPEN_SWITCH, remove, &blk
    end
    def on_editor_line(remove = false, &blk)
      add_event_handler EVENT_EDITOR_LINE, remove, &blk
    end
    def on_output_line(remove = false, &blk)
      add_event_handler EVENT_OUTPUT_LINE, remove, &blk
    end
    def strip_show(s, &blk)
      @event_handlers[EVENT_STRIP] = []
      add_event_handler EVENT_STRIP, false, &blk if blk
      SciTE.strip_show_intern(s)
    end

    # the handler is always reset!
    def user_list_show(list, start = 0, tp = 13, &blk)
      sep = ';'
      s = list[start..-1].join(sep)
      add_event_handler EVENT_USER_LIST_SELECTION, true, &blk
      pane = Editor.focus ? Editor : Output
      pane.autoCSeparator = sep.bytes[0]
      pane.userListShow(tp, s)
      pane.autoCSeparator = " ".bytes[0]
    end

    def current_file
      Props['FilePath']
    end
    
    def load_scripts
      if Module.const_defined?(:Dir) && Kernel.respond_to?(:load)
        if Props["PLAT_WIN"].to_i == 1 then
          path = "#{Props['SciteDefaultHome']}/scite_mruby"
          if Dir.exist?(path)
            Dir.foreach(path) do |file|
              begin
                load path + "/" + file if file[-3..-1] == ".rb"
              rescue => e
                puts e
              end
            end
          end
        end
        path = Props["ext.mruby.directory"]
        if path.to_s == ""
          path = "#{Props['SciteUserHome']}/scite_mruby"
        end
        if Dir.exist?(path)
          Dir.foreach(path) do |file|
            begin
              load path + "/" + file if file[-3..-1] == ".rb"
            rescue => e
              puts e
            end
          end
        end
      end
    end

    def on_buffer_switch(file)
      if file[-1] != "\\" && file[-1] != '/'
        dispatch_one event_handlers[EVENT_OPEN_SWITCH], file
      end
      false
    end

    def grab_line_from(pane)
      line_pos = pane.currentPos
      lineno = pane.lineFromPosition(line_pos)-1
      # strip linefeeds (Windows is a special case as usual!)
      endl = pane.EOLMode == 0 ? 3 : 2
      return pane.getLine(lineno)[0..-endl]
    end

    def on_line_char(ch)
      return false if ch != "\n"
      editor_focused = Editor.focus
      if editor_focused
        return false if !event_handlers[EVENT_EDITOR_LINE] || event_handlers[EVENT_EDITOR_LINE].length == 0
        dispatch_one event_handlers[EVENT_EDITOR_LINE], grab_line_from(Editor)
        false
      else
        return false if !event_handlers[EVENT_OUTPUT_LINE] || event_handlers[EVENT_OUTPUT_LINE].length == 0
        dispatch_one event_handlers[EVENT_OUTPUT_LINE], grab_line_from(Output)
        true
      end
    end

    attr_accessor :event_handlers

    alias_method :sendEditor, :send_editor
    alias_method :sendOutput, :send_output
    alias_method :constantName, :constant_name
    alias_method :menuCommand, :menu_command
    alias_method :updateStatusBar, :update_status_bar
    alias_method :stripShow, :strip_show
    alias_method :stripSet, :strip_set
    alias_method :stripSetList, :strip_set_list
    alias_method :stripValue, :strip_value

    alias_method :loadScripts, :load_scripts
    alias_method :currentFile, :current_file
    alias_method :userListShow, :user_list_show

    alias_method :onMarginClick, :on_margin_click
    alias_method :onDoubleClick, :on_double_click
    alias_method :onSavePointLeft, :on_save_point_left
    alias_method :onSavePointReached, :on_save_point_reached
    alias_method :onOpen, :on_open
    alias_method :onSwitchFile, :on_switch_file
    alias_method :onSaveBefore, :on_save_before
    alias_method :onSave, :on_save
    alias_method :onUpdateUI, :on_update_ui
    alias_method :onChar, :on_char
    alias_method :onKey, :on_key
    alias_method :onDwellStart, :on_dwell_start
    alias_method :onClose, :on_close
    alias_method :onOpenSwitch, :on_open_switch
    alias_method :onEditorLine, :on_editor_line
    alias_method :onOutputLine, :on_output_line

  end

  class StylingContext
    alias_method :startStyling, :start_styling
    alias_method :endStyling, :end_styling
    alias_method :atLineStart, :at_line_start
    alias_method :atLineEnd, :at_line_end
    alias_method :setState, :set_state
    alias_method :forwardSetState, :forward_set_state
    alias_method :changeState, :change_state
  end

  self.on_open { |file| on_buffer_switch file }
  self.on_switch_file { |file| on_buffer_switch file }
  self.on_char { |file| on_line_char file }

end

def on_margin_click
  return onMarginClick if respond_to?(:onMarginClick)
  SciTE.dispatch_one SciTE.event_handlers[SciTE::EVENT_MARGIN_CLICK]
end
def on_double_click
  return onDoubleClick if respond_to?(:onDoubleClick)
  SciTE.dispatch_one SciTE.event_handlers[SciTE::EVENT_DOUBLE_CLICK]
end
def on_save_point_left
  return onSavePointLeft if respond_to?(:onSavePointLeft)
  SciTE.dispatch_one SciTE.event_handlers[SciTE::EVENT_SAVE_POINT_LEFT]
end
def on_save_point_reached
  return onSavePointReached if respond_to?(:onSavePointReached)
  SciTE.dispatch_one SciTE.event_handlers[SciTE::EVENT_SAVE_POINT_REACHED]
end
def on_char(ch)
  return onChar(ch) if respond_to?(:onChar)
  SciTE.dispatch_one SciTE.event_handlers[SciTE::EVENT_CHAR], ch
end
def on_save(file)
  return onSave(file) if respond_to?(:onSave)
  SciTE.dispatch_one SciTE.event_handlers[SciTE::EVENT_SAVE], file
end
def on_before_save(file)
  return onBeforeSave(file) if respond_to?(:onBeforeSave)
  SciTE.dispatch_one SciTE.event_handlers[SciTE::EVENT_BEFORE_SAVE], file
end
def on_switch_file(file)
  return onSwitchFile(file) if respond_to?(:onSwitchFile)
  SciTE.dispatch_one SciTE.event_handlers[SciTE::EVENT_SWITCH_FILE], file
end
def on_open(file)
  return onOpen(file) if respond_to?(:onOpen)
  SciTE.dispatch_one SciTE.event_handlers[SciTE::EVENT_OPEN], file
end
def on_update_ui
  return onUpdateUI if respond_to?(:onUpdateUI)
  if Editor.focus
    SciTE.dispatch_one SciTE.event_handlers[SciTE::EVENT_UPDATE_UI]
  end
end
def on_key(key, shift, ctrl, alt)
  return onKey(key, shift, ctrl, alt) if respond_to?(:onKey)
  SciTE.dispatch_one SciTE.event_handlers[SciTE::EVENT_KEY], key, shift, ctrl, alt
end
def on_dwell_start(pos, s)
  return onDwellStart(pos, s) if respond_to?(:onDwellStart)
  SciTE.dispatch_one SciTE.event_handlers[SciTE::EVENT_DWELL_START], pos, s
end
def on_close(file)
  return onClose(file) if respond_to?(:onClose)
  SciTE.dispatch_one SciTE.event_handlers[SciTE::EVENT_CLOSE], file
end
def on_user_list_selection(tp, str)
  return onUserListSelection(tp, str) if respond_to?(:onUserListSelection)
  SciTE.dispatch_one SciTE.event_handlers[SciTE::EVENT_USER_LIST_SELECTION], str, tp
end
def on_strip(control, change)
  return onStrip(file) if respond_to?(:onStrip)
  SciTE.dispatch_one SciTE.event_handlers[SciTE::EVENT_STRIP], control, change
end

SciTE.load_scripts

SciTE.define_command "Run as mruby script", "*", "Alt+Ctrl+R" do
  if Editor.lexer_language != "ruby"
    SciTE.menu_command IDM_LANGUAGE + 23
  end
  if !Editor.modify && Kernel.respond_to?(:load)
    current_file = SciTE.current_file
    puts "Loading... " + current_file
    begin
      load current_file
    rescue => e
      p e
    end
  else
    eval Editor.get_text
  end
end

