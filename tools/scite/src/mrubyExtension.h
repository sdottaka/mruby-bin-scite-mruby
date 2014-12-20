// SciTE - Scintilla based Text Editor
// mrubyExtension.h - mruby scripting extension based on (LuaExtension: Copyright 1998-2000 by Neil Hodgson <neilh@scintilla.org>)
// The License.txt file describes the conditions under which this software may be distributed.

class mrubyExtension : public Extension {
private:
	mrubyExtension(); // Singleton
	mrubyExtension(const mrubyExtension &);   // Disable copy ctor
	void operator=(const mrubyExtension &); // Disable operator=

public:
	static mrubyExtension &Instance();

	virtual ~mrubyExtension();

	virtual bool Initialise(ExtensionAPI *host_);
	virtual bool Finalise();
	virtual bool Clear();
	virtual bool Load(const char *filename);

	virtual bool InitBuffer(int);
	virtual bool ActivateBuffer(int);
	virtual bool RemoveBuffer(int);

	virtual bool OnOpen(const char *filename);
	virtual bool OnSwitchFile(const char *filename);
	virtual bool OnBeforeSave(const char *filename);
	virtual bool OnSave(const char *filename);
	virtual bool OnChar(char ch);
	virtual bool OnExecute(const char *s);
	virtual bool OnSavePointReached();
	virtual bool OnSavePointLeft();
	virtual bool OnStyle(unsigned int startPos, int lengthDoc, int initStyle, StyleWriter *styler);
	virtual bool OnDoubleClick();
	virtual bool OnUpdateUI();
	virtual bool OnMarginClick();
	virtual bool OnUserListSelection(int listType, const char *selection);
	virtual bool OnKey(int keyval, int modifiers);
	virtual bool OnDwellStart(int pos, const char *word);
	virtual bool OnClose(const char *filename);
	virtual bool OnUserStrip(int control, int change);
};
