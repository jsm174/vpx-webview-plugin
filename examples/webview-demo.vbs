' WebView plugin demo. Copy the examples folder next to the table (fx/, overlay/, sound/, logic/)
' and add this script to the table. The flasher showing screen 1 needs Render Mode "Display" and the
' image source link ctrl://webview/display?id=1. Screen 2 goes to the backglass window when it is enabled.
' Call WebViewDemo_KeyDown keycode from Table1_KeyDown.

Dim WebView, WebViewScore, WebViewHue
Set WebView = CreateObject("WebView.Controller")
WebView.EventHandler = "WebView_OnEvent"
WebViewScore = 0
WebViewHue = 200

WebView.Screen 1, 512, 256
WebView.Screen 2, 1280, 1024
WebView.ShowScreen 2, "backglass"

WebView.Open "fx", "fx/index.html"
WebView.Place "fx", 1, 0, 0, 512, 256
WebView.Place "fx", 2, 0, 0, 1280, 1024

WebView.Open "overlay", "overlay/index.html"
WebView.Place "overlay", 1, 0, 168, 512, 80
WebView.Place "overlay", 2, 0, 820, 1280, 200
WebView.SetLayer "overlay", 1

WebView.Open "sound", "sound/index.html"
WebView.Open "logic", "logic/index.html"

Sub WebViewDemo_KeyDown(keycode)
	If keycode = LeftFlipperKey Then
		WebView.Send "fx", "burst", "1"
		WebView.Send "sound", "play", "whoosh"
		WebViewScore = WebViewScore + 1000
		WebView.Send "overlay", "score", CStr(WebViewScore)
	End If
	If keycode = RightFlipperKey Then
		WebViewHue = (WebViewHue + 60) Mod 360
		WebView.Send "fx", "color", CStr(WebViewHue)
		WebView.Send "sound", "play", "blip"
	End If
End Sub

Sub WebView_OnEvent(page, name, data)
	If name = "loaded" And page = "overlay" Then WebView.Send "overlay", "score", "0"
	If name = "raises" Then
		Debug.Print "left flipper raised " & data & " times"
		If data Mod 10 = 0 Then WebView.Send "sound", "play", "chime" : WebView.Send "fx", "burst", "2"
	End If
End Sub
