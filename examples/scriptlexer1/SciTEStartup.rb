#  -*- coding. utf-8 -*-

def onStyle(styler)
        S_DEFAULT = 0
        S_IDENTIFIER = 1
        S_KEYWORD = 2
        S_UNICODECOMMENT = 3
        identifierCharacters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"

        styler.startStyling(styler.startPos, styler.lengthDoc, styler.initStyle)
        while styler.more do

                # Exit state if needed
                if styler.state == S_IDENTIFIER 
                        if !identifierCharacters.index(styler.current)
                                identifier = styler.token
                                if identifier == "if" || identifier == "end" 
                                        styler.changeState(S_KEYWORD)
                                end
                                styler.setState(S_DEFAULT)
                        end
                elsif styler.state == S_UNICODECOMMENT
                        if styler.match("»")
                                styler.forwardSetState(S_DEFAULT)
                        end
                end

                # Enter state if needed
                if styler.state == S_DEFAULT
                        if styler.match("«")
                                styler.setState(S_UNICODECOMMENT)
                        elsif identifierCharacters.index(styler.current) 
                                styler.setState(S_IDENTIFIER)
                        end
                end

                styler.forward
        end
        styler.endStyling
end

