
# A line oriented lexer - style the line according to the first character
def onStyle(styler)
        lineStart = Editor.lineFromPosition(styler.startPos)
        lineEnd = Editor.lineFromPosition(styler.startPos + styler.lengthDoc)
        Editor.startStyling(styler.startPos, 31)
        for line in lineStart..lineEnd do
                lengthLine = Editor.positionFromLine(line+1) - Editor.positionFromLine(line)
                lineText = Editor.getLine(line)
                first = lineText[0]
                style = 0
                if first == "+"
                        style = 1
                elsif first == " " || first == "\t"
                        style = 2
                end
                Editor.setStyling(lengthLine, style)
        end
end

