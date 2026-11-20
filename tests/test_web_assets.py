import gzip
from html.parser import HTMLParser
from pathlib import Path
import re
import unittest
ROOT=Path(__file__).resolve().parents[1]
class WebAssets(unittest.TestCase):
    def test_packed_sources_match(self):
        root=ROOT/'src/esp32-rfid/web'
        for name in ('app.js','index.html','login.html','style.css'):
            data=bytes(int(b,16) for b in re.findall(r'0x([0-9A-Fa-f]{2})',(root/(name+'.gz.h')).read_text()))
            self.assertEqual(gzip.decompress(data).decode().replace('\r\n','\n'),(root/name).read_text().replace('\r\n','\n'))
    def test_rtc_disable_keeps_save_button_visible(self):
        class Parser(HTMLParser):
            def __init__(self):super().__init__();self.stack=[];self.parents=None
            def handle_starttag(self,tag,attrs):
                attributes=dict(attrs)
                if attributes.get('id')=='rtc-save':self.parents=list(self.stack)
                if tag not in ('input','meta','link','br','img','hr'):self.stack.append((tag,attributes.get('id')))
            def handle_endtag(self,tag):
                for n in range(len(self.stack)-1,-1,-1):
                    if self.stack[n][0]==tag:self.stack=self.stack[:n];break
        parser=Parser();parser.feed((ROOT/'src/esp32-rfid/web/index.html').read_text())
        self.assertIsNotNone(parser.parents)
        self.assertNotIn('rtc-config',[node[1] for node in parser.parents])
if __name__=='__main__':unittest.main(verbosity=2)
