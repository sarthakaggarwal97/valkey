#!/usr/bin/env python3
"""Records golden results for the path tests from a reference server.

The reference is a Valkey server with the external valkey-json module loaded,
listening on a private unix socket (see run_ref.sh). For every (document, path)
pair this script records what the module does:

  GET        JSON.GET doc path
  SET        JSON.SET doc path '"NEW"', then the whole document
  DEL        JSON.DEL doc path, then the whole document
  ARRAPPEND  JSON.ARRAPPEND doc path 999, then the whole document
             (not for paths with "..": nested matches there read freed memory
             in the module, design section 2.5)
  ROOM       for a path JSON.SET accepts, the deepest value it still accepts,
             found by bisection. It is 128 minus the selector's path depth.

Each write runs on a fresh copy of the document. The output is read by
path_test.c. Usage: gen_corpus.py SOCKET MODULE_TEST_DIR OUT.gz
"""
import ast
import gzip
import json
import random
import re
import socket
import sys


class Resp:
    """A minimal RESP2 client, so that error text arrives untouched."""

    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(30)  # The module loops for ever on some slice steps: fail instead of waiting.
        self.s.connect(path)
        self.f = self.s.makefile("rb")

    def cmd(self, *args):
        out = b"*%d\r\n" % len(args)
        for a in args:
            if isinstance(a, str):
                a = a.encode("utf-8", "surrogateescape")
            out += b"$%d\r\n%s\r\n" % (len(a), a)
        self.s.sendall(out)
        return self.read()

    def read(self):
        line = self.f.readline()
        if not line:
            raise ConnectionError("server closed the connection")
        kind, rest = line[:1], line[1:-2]
        if kind == b"+":
            return ("S", rest)
        if kind == b"-":
            return ("E", rest)
        if kind == b":":
            return ("I", rest)
        if kind == b"$":
            n = int(rest)
            if n < 0:
                return ("N", b"")
            data = self.f.read(n + 2)[:-2]
            return ("S", data)
        if kind == b"*":
            n = int(rest)
            if n < 0:
                return ("N", b"")
            items = [self.read() for _ in range(n)]
            return ("A", b"[" + b",".join(b"null" if k == "N" else v for k, v in items) + b"]")
        raise ValueError("unexpected reply %r" % line)


def enc(reply):
    return reply[0].encode() + reply[1]


# ---------------------------------------------------------------------------
# Documents
# ---------------------------------------------------------------------------

STORE = """{"store":{"books":[
{"category":"reference","author":"Nigel Rees","title":"Sayings of the Century","price":8.95,"in-stock":true,"sold":true},
{"category":"fiction","author":"Evelyn Waugh","title":"Sword of Honour","price":12.99,"in-stock":true,"sold":false,"movies":[{"title":"Sword of Honour","realisator":{"first_name":"Bill","last_name":"Anderson"}}]},
{"category":"fiction","author":"Herman Melville","title":"Moby Dick","isbn":"0-553-21311-3","price":9,"in-stock":false,"sold":true},
{"category":"fiction","author":"J. R. R. Tolkien","title":"The Lord of the Rings","isbn":"0-115-03266-2","price":22.99,"in-stock":true,"sold":false},
{"category":"reference","author":"William Jr. Strunk","title":"The Elements of Style","price":6.99,"in-stock":false,"sold":true}],
"bicycle":{"color":"red","price":19.95,"in-stock":true,"sold":false}},
"budget":10.00,"expensive":10,"firstName":"John","lastName":"Smith","age":27,"weight":135.17,"isAlive":true,
"address":{"street":"21 2nd Street","city":"New York","state":"NY","zipcode":"10021-3100"},
"phoneNumbers":[{"type":"home","number":"212 555-1234"},{"type":"office","number":"646 555-4567"}],
"children":[],"spouse":null,"groups":{}}"""

DOCS = [
    STORE,
    '{"a":[1,2,3,4,5],"b":{"c":5,"a":{"a":1}},"e":[],"o":{},"s":"str","n":null,"t":true,"f":false,"x":1.5,'
    '"":7,"a b":1,"k~1":2,"$x":3,"*":4,"a.b":5,"a/b":6,"0":8,"new":9,"a\'b":10,"q\\"r":11}',
    '[[1,2,3],[4,5],[],[[6,7],[8]],{"a":[9]},"s",0]',
    '{"a":{"a":{"a":1,"b":[{"a":2},{"b":3}]},"b":{"a":[{"a":{"a":4}}]}},"c":[{"a":5},[{"a":6}]]}',
    '[{"a":1,"b":"x","c":[1,2,3]},{"a":2,"b":"y","c":[4,5]},{"a":3},{"b":"z","c":[]},5,"s",null,[1,2],'
    '{"a":"1","c":{"d":1}},{"a":2.0,"b":"x","c":["x","y"]},true,{"":1,"a":null}]',
    '{"v":[0,1,-1,1.0,1.5,2,1e2,100,9223372036854775807,9223372036854775808,18446744073709551615,'
    '-9223372036854775808,0.1,-0.0,"1",true,false,null,[],{},123456789012345678901234567890,2.50,1E2]}',
    '{"v":["","a","b","ab","B","\\u00e9","a\\"b","a\'b","a\\\\b","\\n","abc","true","null","1"],'
    '"w":[{"n":"a"},{"n":"b"},{"n":""},{"n":"a\'b"},{"n":"q\\"r"},{"n":true},{"n":false},{"n":null}]}',
    "5",
    '"abc"',
    "null",
    "true",
    "[]",
    "{}",
    "[" + ",".join('{"x":{"a":%d},"i":%d,"l":[%d,[%d]]}' % (i, i, i, i) for i in range(30)) + "]",
    '{"d":' + "[" * 110 + "1" + "]" * 110 + ',"m":' + '{"k":' * 60 + "2" + "}" * 60 + "}",
]

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

HAND = r"""
$
.

$.
$$
$.$
..
$..
$...
....
$..a
$..a..a
$..a..b
$..b
$..*
$..[*]
$..[0]
$..[0,0]
$..[-1]
$..[?(@.a)]
$..[?(@.a>1)]
$..a[?(@.a)]
$..['a','b']
..a
..a.a
...a
..$.a
..$
$..$
$..$.a
$.a..
$.a...a
$..a.b..a
$..x..a
$.a
$.a.a
$.a.a.a
$.a.a.a.a
$.b
$.b.c
$.b.a.a
$.zz
$.zz.yy
$.zz.yy.xx
$.new
$.newkey
$.b.newkey
$.b.zz.yy
$.a.newkey
$.s.newkey
$.e.newkey
$.newkey]
$.newkey[
$.newkey.
$.newkey
$.newkey..
$.newkey*
$.newkey[0]
$['newkey']
$['newkey'].
$['newkey'].x
$['newkey']
$["newkey"]
$['newkey','b']
$.*.newkey
$[*].newkey
$..newkey
$.b~1c
$.b~0c
$.b/c
$.k~1
$['k~1']
$['a/b']
$.a/b
$.a.b
$['a.b']
$.a b
$['a b']
$["a b"]
$.a'b
$['a\'b']
$["a'b"]
$['q"r']
$["q\"r"]
$["q\\"]
$["q\\" ]
$['']
$[""]
$.''
$.$x
$['$x']
$.*
$.*.*
$.*.*.*
$[*]
$[*][*]
$[*].*
$.*[*]
$.*.a
$.*a
$.**
$.*.
$[*].
$[*]..
$[*]$
$.*$
$[*].$
$.a*
$.a[*]
$.a.*
$.b.*
$.s.*
$.s[*]
$.n.*
$.o.*
$.e[*]
.*
.*.*
.a.*
.s.*
.a[*]
[*]
*
*.a
$[ * ]
$[* ]
$[ *]
$[*] .a
$[*]. a
$.a[0]
$.a[1]
$.a[4]
$.a[5]
$.a[-1]
$.a[-5]
$.a[-6]
$.a[+1]
$.a[ 1 ]
$.a[1 ]
$.a[ 1]
$.a[01]
$.a[1x]
$.a[x]
$.a[- 1]
$.a[--1]
$.a[1.0]
$.a[]
$.a[
$.a[1
$.a]
$.a[0].x
$.b[0]
$.s[0]
$.e[0]
$[0]
$[1]
$[0][0]
$[0][1]
$[3][0][1]
$[0].a
$[0].c[1]
$[3]
[0]
.[0]
$.[0]
$.a.[0]
$..[1]
.a[0]
.a[5]
.a[-1]
.b[0]
.a.b
.zz
.zz.yy
.b.zz
.a[0].b
a
a.a
a[0]
b.c
store.books[0].title
$.a[0:2]
$.a[1:]
$.a[:2]
$.a[:]
$.a[::]
$.a[::2]
$.a[1::2]
$.a[:3:2]
$.a[1:4:2]
$.a[-2:]
$.a[:-2]
$.a[-3:-1]
$.a[3:1]
$.a[4:1:-1]
$.a[5:0:-1]
$.a[::-1]
$.a[4::-1]
$.a[-1::-1]
$.a[0:100]
$.a[-100:100]
$.a[100:]
$.a[1:2:0]
$.a[::0]
$.a[ 1 : 3 ]
$.a[1 :3]
$.a[1: 3]
$.a[1:3 ]
$.a[ : ]
$.a[ :2]
$.a[: 2]
$.a[1:3: 1]
$.a[1:3 :1]
$.a[1:3:1 ]
$.a[::1 ]
$.a[::1 ]
$.a[::1 ] .x
$.a[1:x]
$.a[x:1]
$.a[1:2:x]
$.a[1:2:3:4]
$.a[1:
$.a[1:2
$.a[1:2:
$.a[:
$.a[::
$.b[0:2]
$.b[:]
$.b[1:x]
$.b[:x]
$.s[0:1]
$.e[:]
$[0:2]
$[1:3][0]
$[:][0]
$[0:2].a
$[:].c[0]
.a[0:2]
.a[:]
.b[:]
.a[1:x]
.b[1:x]
$.a[0,1]
$.a[0,2,4]
$.a[4,0]
$.a[0,0]
$.a[0,-1]
$.a[-1,-2]
$.a[0,10]
$.a[10,0]
$.a[10,11]
$.a[0, 1]
$.a[0 ,1]
$.a[ 0 , 1 ]
$.a[0,1 ]
$.a[0,,1]
$.a[0,]
$.a[,0]
$.a[0,1,]
$.a[0 1]
$.a[0, 1 2]
$.a[0,x]
$.a[0,1
$.a[0,
$.e[0,1]
$.e[0]
$.b[0,1]
$.b[0,x]
$.s[0,1]
$[0,1]
$[0,1][0]
$[0,1,2][0]
$[2,1,0][0]
$[0,2][5]
$[1,0].a
.a[0,1]
.a[1,0]
.e[0,1]
.b[0,1]
$['a']
$["a"]
$['a']['a']
$['b']['c']
$['a'][0]
$['a','b']
$['b','a']
$['a','a']
$['a','zz']
$['zz','yy']
$["a","b"]
$['a',"b"]
$['a' , 'b']
$[ 'a','b' ]
$['a','b' ]
$['a','b'].c
$['a','b'][0]
$['a','b'].a
$['a',]
$[,'a']
$['a''b']
$['a' 'b']
$['a
$['a'
$["a
$["a"
$['a']x
$['a'].
$[a]
$[ 'a' ]
$[ "a" ]
$['a' ]
$[ 'a']
$.b['c']
$.b['c','a']
$.s['a']
$.s['a','b']
$.a['a']
$.a['a','b']
['a']
.['a']
$.['a']
$.b.['c']
$..['a']
$["a"]
$["é"]
$["a\nb"]
$["😀"]
$["\ud83d"]
$["\x"]
$['\x']
$['a\\']
$['a\tb']
$['\'']
$[?(@.a)]
$[?(@.c)]
$[?(@.zz)]
$[?(@.a==1)]
$[?(@.a==2)]
$[?(@.a==2.0)]
$[?(@.a!=2)]
$[?(@.a>1)]
$[?(@.a>=2)]
$[?(@.a<2)]
$[?(@.a<=2)]
$[?(@.a=="1")]
$[?(@.a=='1')]
$[?(@.a==null)]
$[?(@.a!=null)]
$[?(@.a==true)]
$[?(@.b=="x")]
$[?(@.b=='x')]
$[?(@.b!="x")]
$[?(@.b>"x")]
$[?(@.b>="y")]
$[?(@.b<"y")]
$[?(@.b=="x")].a
$[?(@.a>1)].b
$[?(@.a>1)].c[0]
$[?(@.a>1)].c[*]
$[?(@.a>1)][0]
$[?(@.a>1)]..a
$[?(@.a>1)].
$[?(@.a>1)]$
$[?(@.a>1)].$
$[?(@.a>1)] .b
$[?(@.a>1) ].b
$[?(@.a>1) ] .b
$[?(@.a>1)].  b
$[?(@.a>1)]b
$[ ?(@.a>1)]
$[? (@.a>1)]
$[?( @.a>1)]
$[?(@ .a>1)]
$[?(@. a>1)]
$[?(@.a >1)]
$[?(@.a> 1)]
$[?(@.a>1 )]
$[?(@.a > 1 )]
$[?(  @.a  >  1  )  ]
$[?(@.a>1)
$[?(@.a>1
$[?(@.a>
$[?(@.a
$[?(@.
$[?(@
$[?(
$[?
$[?]
$[?()]
$[?(@)]
$[?(@.a>)]
$[?(@.a>>1)]
$[?(@.a=1)]
$[?(@.a===1)]
$[?(@.a<>1)]
$[?(@.a!1)]
$[?(@.a>1))]
$[?((@.a>1)]
$[?((@.a>1))]
$[?(((@.a>1)))]
$[?( ( @.a>1 ) )]
$[?(@.a>1&&@.b=="x")]
$[?(@.a>1 && @.b=="y")]
$[?(@.a>1||@.b=="x")]
$[?(@.a>1 || @.b=="x")]
$[?(@.b=="x" || @.a>1)]
$[?(@.a==3 || @.a==1 || @.a==2)]
$[?(@.a==2 || @.a==1)]
$[?(@.a>1&&@.b)]
$[?(@.a && @.b && @.c)]
$[?(@.a || @.c)]
$[?(@.c || @.a)]
$[?(@.a>1 && (@.b=="x" || @.b=="y"))]
$[?((@.a>1 && @.b=="x") || @.c)]
$[?(@.a>1 & @.b)]
$[?(@.a>1 | @.b)]
$[?(@.a>1 &&)]
$[?(&& @.a>1)]
$[?(@.a>1 || )]
$[?(!@.a)]
$[?(@.a>1 and @.b)]
$[?(1<@.a)]
$[?(1 < @.a)]
$[?(2<=@.a)]
$[?(2>=@.a)]
$[?(2>@.a)]
$[?(2==@.a)]
$[?(2!=@.a)]
$[?("x"==@.b)]
$[?('x'==@.b)]
$[?(null==@.a)]
$[?(true==@.a)]
$[?(1<@)]
$[?(1 < @ )]
$[?(1<@ && @<5)]
$[?(1<@.a && @.a<3)]
$[?(1<@.c[0])]
$[?(1<@.c[-1])]
$[?(1<@.c[x])]
$[?(1<@.c[0)]
$[?(1<)]
$[?(1<@.)]
$[?(1 @.a)]
$[?(1<@a)]
$[?(@>1)]
$[?(@>=5)]
$[?(@==5)]
$[?(@=="s")]
$[?(@==null)]
$[?(@==true)]
$[?(@!=5)]
$[?(@ > 1)]
$[?(@==1)]
$[?(@.c[0]==1)]
$[?(@.c[0]>1)]
$[?(@.c[-1]>=3)]
$[?(@.c[1]==5)]
$[?(@.c[5]==5)]
$[?(@.c[0]=="x")]
$[?(@.c[ 0]==1)]
$[?(@.c[0 ]==1)]
$[?(@.c[0] ==1)]
$[?(@.c[0]== 1)]
$[?(@.c[x]==1)]
$[?(@.c[0]]
$[?(@.c[0])]
$[?(@.c[0]=)]
$[?(@.c[?(@==1)])]
$[?(@.c[?(@>2)])]
$[?(@.c[?(@=="x")])]
$[?(@.c[?(1==@)])]
$[?(@.c[?(2<@)])]
$[?(@.c[ ?(@>2)])]
$[?(@.c[? (@>2)])]
$[?(@.c[?( @>2)])]
$[?(@.c[?(@ >2)])]
$[?(@.c[?(@>2 )])]
$[?(@.c[?(@>2) ])]
$[?(@.c[?(@>2)] )]
$[?(@.c[?(@>2)]) ]
$[?(@.c[?(@>2)]
$[?(@.c[?(@>2)
$[?(@.c[?(@>2
$[?(@.c[?(@
$[?(@.c[?(
$[?(@.c[?
$[?(@.c[?(@.a>2)])]
$[?(@.c[?(@>2)] && @.a)]
$[?(@['a'])]
$[?(@["a"])]
$[?(@['a']>1)]
$[?(@["a"]>1)]
$[?(@[ 'a' ]>1)]
$[?(@.['a']>1)]
$[?(@['a'] > 1 && @['b'] == "y")]
$[?(@['a','b'])]
$[?(@[a])]
$[?(@[0])]
$[?(@[0]>1)]
$[?(@['c'][0]==1)]
$[?(@.a.b)]
$[?(@.c.d)]
$[?(@.c.d==1)]
$[?(@..a)]
$[?(@.*)]
$[?(@.a>1.5)]
$[?(@.a>1e0)]
$[?(@.a>1E0)]
$[?(@.a>-1)]
$[?(@.a>+1)]
$[?(@.a>.5)]
$[?(@.a>1.)]
$[?(@.a>01)]
$[?(@.a>1e)]
$[?(@.a>e1)]
$[?(@.a>1e400)]
$[?(@.a>-)]
$[?(@.a>--1)]
$[?(@.a>1-1)]
$[?(@.a>0x1)]
$[?(@.a>abc)]
$[?(@.a==nul)]
$[?(@.a==nullx)]
$[?(@.a==Null)]
$[?(@.a==tru)]
$[?(@.a==truex)]
$[?(@.a==True)]
$[?(@.a==false)]
$[?(@.a==fals)]
$[?(@.a==n)]
$[?(@.a==t)]
$[?(@.a==f)]
$[?(@.a==x)]
$[?(@.a=="1)]
$[?(@.a=='1)]
$[?(@.a=="1")]
$[?(@.a=='1')]
$[?(@.a=="\x")]
$[?(@.a=='\x')]
$[?(@.b=="x" )]
$[?(@.b== "x")]
$[?(@.b=='x' )]
$[?(@.a==$.zz)]
$[?(@.a==$[0].a)]
$[?(@.a>$[0].a)]
$[?(@.a>$[0]['a'])]
$[?(@.a>$[0]["a"])]
$[?(@.a>=$[1].a)]
$[?(@.a<$[2].a)]
$[?(@.a==$[0])]
$[?(@.a==$[0].c)]
$[?(@.a==$[*].a)]
$[?(@.a==$..d)]
$[?(@.a==$[4])]
$[?(@.a>$[99])]
$[?(@.a>$[0].c[0])]
$[?(@.a>$[0].c[-1])]
$[?(@.a>$[0].c[x])]
$[?(@.a>$[0].c[1:2])]
$[?(@.a>$[0].c[*])]
$[?(@.a>$[?(@.a)])]
$[?(@.a>$)]
$[?(@.a>$.)]
$[?(@.a>$[)]
$[?(@.a>$[0)]
$[?(@.a>$[0] )]
$[?(@.a>$[0].a )]
$[?(@.a> $[0].a)]
$[?($[0].a<@.a)]
$[?($[0].a < @.a)]
$[?($[1].a==@.a)]
$[?($[0].a<@)]
$[?($.zz<@.a)]
$[?($[0].a @.a)]
$[?($[0].a<)]
$[?(@.b==$[0].b)]
$[?(@.a>1)][?(@.b)]
$[*][?(@.a>1)]
$[*][?(@>1)]
$[*][ ?(@.a>1)]
$[*] [?(@.a>1)]
$[ * ][?(@.a>1)]
$[*][?(@.a>1) ]
$[*][?(@.a>1 )]
$[*][?( @.a>1)]
$[*][? (@.a>1)]
$[*][?(@.a>1)].b
$[*][?(@.a>1)] .b
$[*][?(@.a>1)
$.*[?(@.a>1)]
$.*.[?(@.a>1)]
$.* [?(@.a>1)]
$.*[ ?(@.a>1)]
$.*[?( @.a>1)]
$.*[? (@.a>1)]
$.*[?(@.a>1 )]
$.*[?(@.a>1) ]
$.*[?(@.a>1)]
$.*[?(@.a>1)].b
$.*[?(@.a>1)] .b
$.*[?(@>1)]
$.*[?(@.a>x)]
$.*[?(@.a>1)
$.a[?(@>2)]
$.a[?(@>2 && @<5)]
$.a[?(@==3 || @==1)]
$.a[?(@>2)][0]
$.a[?(@>2)].x
$.a[?(@.x)]
$.a.*[?(@>2)]
$.a[*][?(@>2)]
$.b[?(@.c)]
$.b[?(@.c==5)]
$.b[?(@.c==5)].c
$.b[?(@.c==5)].a.a
$.b[?(@.c==6)].c
$.b[?(@.zz)].c
$.b[?(@==5)]
$.b[?(@.a)]
$.b[?(@.a)].a[?(@.a)]
$.b[?(@.a)].a[?(@.a)].a
$.b[?(@.c)]$x
$.b[?(@.c)].$x
$.b[?(@.c)] $x
$.b[?(@.c)].  c
$.b[?(@.c) ].c
$.s[?(@.x)]
$.s[?(@=="str")]
$.s[?(@=="str")].x
$.s[?(@=="str")][0]
$.s[?(@=="str")]$
$.s[?(@=="str")][?(@=="str")]
$.s[?(@!="str")]
$.x[?(@>1)]
$.x[?(@>1.5)]
$.x[?(@>=1.5)]
$.n[?(@==null)]
$.n[?(@!=null)]
$.t[?(@==true)]
$.t[?(@!=false)]
$.t[?(@>false)]
$.f[?(@<true)]
$.t[?(@==1)]
$.e[?(@.a)]
$.o[?(@.a)]
$.o[?(@==1)]
$[?(@==7)]
$[?(@.c==5)]
$[?(@.a)].b
$[?(@.s=="str")].s
$[?(@.s=="str")].newkey
$[?(@.s=="str")]
.a[?(@>2)]
.[?(@.a>1)]
[?(@.a>1)]
.b[?(@.c==5)]
.b[?(@.c==5)].c
.s[?(@=="str")]
.s[?(@=="str")]$
.s[?(@.x)]
..[?(@.a>1)]
$.v[?(@==1)]
$.v[?(@==1.0)]
$.v[?(@==1.5)]
$.v[?(@!=1)]
$.v[?(@>1)]
$.v[?(@>=1)]
$.v[?(@<1)]
$.v[?(@<=1)]
$.v[?(@==0)]
$.v[?(@==-0)]
$.v[?(@==0.0)]
$.v[?(@==-0.0)]
$.v[?(@<0)]
$.v[?(@==100)]
$.v[?(@==1e2)]
$.v[?(@==100.0)]
$.v[?(@==9223372036854775807)]
$.v[?(@==9223372036854775808)]
$.v[?(@>9223372036854775807)]
$.v[?(@>=9223372036854775808)]
$.v[?(@<9223372036854775808)]
$.v[?(@>9223372036854775808)]
$.v[?(@==18446744073709551615)]
$.v[?(@<18446744073709551615)]
$.v[?(@==18446744073709551616)]
$.v[?(@<18446744073709551616)]
$.v[?(@==-9223372036854775808)]
$.v[?(@>-9223372036854775808)]
$.v[?(@<-9223372036854775808)]
$.v[?(@<-9223372036854775809)]
$.v[?(@>-1)]
$.v[?(@>-2)]
$.v[?(@<-0.5)]
$.v[?(@==0.1)]
$.v[?(@>0.1)]
$.v[?(@==2.5)]
$.v[?(@==2.50)]
$.v[?(@>1e30)]
$.v[?(@>1e29)]
$.v[?(@==123456789012345678901234567890)]
$.v[?(@=="1")]
$.v[?(@==true)]
$.v[?(@==false)]
$.v[?(@!=true)]
$.v[?(@>false)]
$.v[?(@>=false)]
$.v[?(@<true)]
$.v[?(@<=true)]
$.v[?(@==null)]
$.v[?(@!=null)]
$.v[?(@>null)]
$.v[?(@>=null)]
$.v[?(@<=null)]
$.v[?(@=="")]
$.v[?(@=='')]
$.v[?(@=="a")]
$.v[?(@>"a")]
$.v[?(@>="a")]
$.v[?(@<"b")]
$.v[?(@<="ab")]
$.v[?(@!="a")]
$.v[?(@>"")]
$.v[?(@>"B")]
$.v[?(@>"z")]
$.v[?(@=="é")]
$.v[?(@=="a\"b")]
$.v[?(@=='a"b')]
$.v[?(@=="a'b")]
$.v[?(@=='a\'b')]
$.v[?(@=="a\\b")]
$.v[?(@=='a\\b')]
$.v[?(@=="\n")]
$.v[?(@=='\n')]
$.v[?(@=="true")]
$.v[?(@==$.v[1])]
$.v[?(@>$.v[1])]
$.v[?(@==$.v[10])]
$.v[?(@==$.v[14])]
$.v[?(@==$.v[15])]
$.v[?(@==$.v[17])]
$.v[?(@==$.v[18])]
$.v[?(@==$.v[19])]
$.v[?(@==$.v)]
$.v[?(@==$.v[0] || @==$.v[1])]
$.v[?($.v[1]<@)]
$.w[?(@.n=="a")]
$.w[?(@.n=='a')]
$.w[?(@.n=="")]
$.w[?(@.n=="a'b")]
$.w[?(@.n=='a\'b')]
$.w[?(@.n=="q\"r")]
$.w[?(@.n=='q"r')]
$.w[?(@.n==true)]
$.w[?(@.n==false)]
$.w[?(@.n!=true)]
$.w[?(@.n==null)]
$.w[?(@.n>"a")]
$.w[?(@.n)]
$.store.books[?(@.price<10)].title
$.store.books[?(@.price<$.expensive)].title
$.store.books[?(@.price<$.budget)].title
$.store.books[?(@.price<$['budget'])].title
$.store.books[?(@.price<$.store.books[1].price)].title
$.store.books[?(@.price<$.store.books[-1].price)].title
$.store.books[?(@.price<$.store.bicycle.price)].title
$.store.books[?(@.isbn)].title
$.store.books[?(@.sold==true)].title
$.store.books[?(@['in-stock']==true)].title
$.store.books[?(@.in-stock==true)].title
$.store.books[?(@.category=='fiction' && @.price<10)].title
$.store.books[?(@.category=='fiction' || @.price<10)].title
$.store.books[?(@.price<10 || @.category=='fiction')].title
$.store.books[?(@.movies)].movies[0].realisator.first_name
$.store.books[?(@.movies[0].title)]
$.store.books[*].title
$.store.books[*]['title','price']
$.store.books[0,2].title
$.store.books[1:3].title
$.store.books[-1].title
$.store.books[-1:].title
$.store.books[:2].price
$.store..price
$.store.*
$.store.books[*].movies[*].realisator.*
$..books[?(@.price>10)].title
$..books[2].title
$..title
$..realisator
$..books[*].isbn
$..['title','price']
$..*.title
$.phoneNumbers[*].type
$.phoneNumbers[?(@.type=="home")].number
$.phoneNumbers[?(@.type=='office')].number
$.address.city
$.address['city','state']
$.children[0]
$.children[*]
$.spouse.x
$.groups.x
$.groups.*
.store.books[0].title
.store.books[*].title
.store.books[-1].price
.store.bicycle.color
.phoneNumbers[0].number
.address.city
.firstName
.children
.spouse
.store..price
..price
.store.books[?(@.price<10)].title
$[*].x.a
$[*].x..a
$[*].x..a..a
$[*]..a
$[*]..i
$..x..a
$..i
$..l[0]
$..l[1][0]
$..l..[0]
$[*].l[1][0]
$[0:25].x..a
$[0:20].x..a
$[0:21].x..a
$[0:19].x..a
$[*].i
$[?(@.i>27)].i
$[?(@.i>27)].x..a
$[29].x.a
$[30].x.a
$[-1].i
$[-30].i
$[-31].i
$.d
$.m
$.d[0][0][0]
$.m.k.k.k
$.m..k
$..k
""" .split("\n")

# Paths that have to be built: nesting limits and long queries.
BUILT = [
    "$.d" + "[*]" * 98,
    "$.d" + "[*]" * 99,
    "$.d" + "[*]" * 100,
    "$.d" + "[*]" * 105,
    "$.d" + "[0]" * 110,
    "$.d" + "[0]" * 111,
    "$.d" + "[0:1]" * 99,
    "$.d" + "[0:1]" * 100,
    "$.d" + "[0,0]" * 5,
    "$.d" + "[*]" * 97 + "[?(@==1)]",
    "$.d" + "[*]" * 98 + "[?(@==1)]",
    "$.d" + "[*]" * 90 + "[?(@==$.d" + "[0]" * 110 + ")]",
    "$.m" + ".k" * 60,
    "$.m" + ".k" * 61,
    "$.m" + ".*" * 60,
    "$.m" + "..k" * 19,
    "$.m" + "..k" * 20,
    "$.m" + "..k" * 21,
    "$" + "..k" * 25,
    "$[?(" + "(" * 60 + "@.a>1" + ")" * 60 + ")]",
    "$[?(" + "(" * 64 + "@.a>1" + ")" * 64 + ")]",
    "$[?(" + "(" * 65 + "@.a>1" + ")" * 65 + ")]",
    "$[?(" + "(" * 66 + "@.a>1" + ")" * 66 + ")]",
    "$[?(" + "(" * 70 + "@.a>1" + ")" * 70 + ")]",
    "$[?(" + "(" * 70 + "@.a>x" + ")" * 70 + ")]",
    "$[?(" + "(" * 300 + "@.a>1" + ")" * 300 + ")]",
    "$[?(" + " || ".join("@.a==%d" % i for i in range(40)) + ")]",
    "$[?(" + " && ".join("@.a>%d" % -i for i in range(40)) + ")]",
    "$.a[" + ",".join(str(i % 5) for i in range(50)) + "]",
    "$['" + "','".join("abc"[i % 3] for i in range(40)) + "']",
    "$." + "a" * 5000,
    "$." + "a" * (128 * 1024 - 2),
    "$." + "a" * (128 * 1024 - 1),
    "$." + "a" * (128 * 1024 + 10),
    "." + "a" * (128 * 1024 - 1),
    "." + "a" * (128 * 1024),
]


# Slice steps at the edges of 32 bits for which the module still gives a
# coherent result: its index is an int, so a step acts by its low 32 bits, and
# these are the steps whose low 32 bits keep the sign of the step and do not
# overflow the index from the starts used here. Steps such as 2147483648 and
# 4294967296 make the module loop for ever and are tested without it, in
# path_test.c. Document 1 has "a":[1,2,3,4,5] and document 2 is an array of 7.
def big_steps():
    out = []
    for step in ("4294967297", "4294967298", "8589934593", "12884901890", "2147483647", "2147483646"):
        for form in ("$.a[0:5:%s]", "$.a[1:5:%s]", "$.a[1::%s]", "$.a[::%s]", ".a[1:5:%s]", "$.a[1:1:%s]"):
            out.append((1, form % step))
        for form in ("$[1:6:%s]", "$[::%s]", "$[0:7:%s][0]"):
            out.append((2, form % step))
    for step in ("-4294967297", "-4294967298", "-8589934593", "-2147483648", "-2147483647"):
        for form in ("$.a[4:0:%s]", "$.a[5:1:%s]", "$.a[-1:0:%s]", "$.a[3::%s]", ".a[4:0:%s]", "$.a[4:-5:%s]"):
            out.append((1, form % step))
        for form in ("$[6:1:%s]", "$[6:0:%s][0]"):
            out.append((2, form % step))
    return out


# Paths that select a value more than once. A read keeps the repeats, except
# after a recursive descent. A write must act on each value once.
REPEATS = """$[1,1,1]
$[0,0,1]
$[0,0][0]
$[0,0].a
$[-1,6,-1]
$.a[1,1,1]
$.a[4,-1,4]
$.a[0,0,0,0,0,0,0,0]
$.a[0:3,1:2]
.a[1,1,1]
$['a','a','b','a']
$['b','b'].c
$['b','b'].a.a
$['b','b'].newkey
$['b','b']['c','c']
$.*[0,0]
$[*][0,0]
$..[0]
$..[0,0]
$..[0]..[0]
$..['a','a']
$..a[0,0]
$..b..a
$..*..a""".split("\n")


# Documents nested deeper than json.max-path-limit allows by default, which a
# server can hold after loading them from a file. The module's nesting limit
# of 200 counts wildcards, slices, unions and filters, two levels each. It
# does not count the levels a recursive descent goes down or the steps of a
# plain chain, so those work at any depth the module's stack survives (it dies
# somewhere between 5,000 and 20,000 levels, which is not recorded here).
DEEP_DOCS = [
    "[" * 300 + "1" + "]" * 300,
    '{"a":' * 300 + "1" + "}" * 300,
    '{"a":[' * 150 + "1" + "]}" * 150,
    "[" * 1000 + "1" + "]" * 1000,
    "[1,[2," * 250 + "3" + "]]" * 250,
]
DEEP_PATHS = [
    "$..*", "$..[0]", "$..[1]", "$..a", "..a", "..[0]", "$..[-1]", "$..[0,0]", "$..['a','a']", "$..[?(@.a)]", "$..[?(@==1)]",
    "$..[?(@>1)]", "$..a..a", "$..[0]..[0]", "$..[1]..[0]",
    "$" + "[0]" * 300, "$" + "[0]" * 301, "." + "[0]" * 300, "$" + ".a" * 300, "$" + ".a" * 301, "a" + ".a" * 299,
    "$" + ".a[0]" * 150, "$" + "[1]" * 500, "$" + "[1]" * 400 + "[0]",
    "$" + "[*]" * 99, "$" + "[*]" * 100, "$" + ".*" * 99, "$" + ".*" * 100, "$" + "[0:1]" * 99, "$" + "[0:1]" * 100,
    "$" + "[0]" * 150 + "[*]" * 99, "$" + "[0]" * 150 + "[*]" * 100, "$" + ".a" * 150 + ".*" * 99, "$" + ".a" * 150 + ".*" * 100,
    "$" + "[*]" * 50 + "..[0]", "$" + "[0]" * 250 + "..*", "$" + ".a" * 250 + "..a", "$" + "[0]" * 250 + "..[0]",
    "$" + "[0,0]" * 8, "$" + "['a','a']" * 8, "$" + "[0]" * 290 + "[0,0]" * 9, "$" + "[1]" * 240 + "..[0]",
    "$" + "[0]" * 200 + "[?(@==1)]", "$" + "[0]" * 299 + "[?(@==1)]", "$" + ".a" * 299 + "[?(@==1)]",
]


def harvest(test_dir):
    """Every path-like string and every small document in the module's own tests."""
    paths, docs = [], []
    src = open(test_dir + "/integration/test_json_basic.py").read()
    for n in ast.walk(ast.parse(src)):
        if isinstance(n, ast.Constant) and isinstance(n.value, str):
            s = n.value
            if s[:1] in ("$", ".", "[") and len(s) < 160 and "\n" not in s and "\x00" not in s:
                paths.append(s)
            t = s.strip()
            if 40 < len(t) < 1500 and t[:1] in "{[":
                try:
                    json.loads(t)
                    docs.append(t)
                except ValueError:
                    pass
    cc = open(test_dir + "/unit/selector_test.cc").read()
    for lit in re.findall(r'"((?:[^"\\\n]|\\.)*)"', cc):
        if lit[:1] in ("$", ".", "["):
            try:
                paths.append(ast.literal_eval('"' + lit + '"'))
            except (SyntaxError, ValueError):
                pass
    for raw in re.findall(r'R"\((.*?)\)"', cc, re.S):
        try:
            json.loads(raw)
            docs.append(raw.strip())
        except ValueError:
            pass
    return paths, docs



SAFE_NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_-]*$")


def lit(v, rng):
    if isinstance(v, str) and rng.random() < 0.3 and "\\" not in v:
        return "'" + v.replace("'", "\\'") + "'"
    return json.dumps(v)


def member(k, rng):
    if SAFE_NAME.match(k) and rng.random() < 0.7:
        return "." + k
    if rng.random() < 0.5 and "\\" not in k:
        return "['" + k.replace("'", "\\'") + "']"
    return "[" + json.dumps(k) + "]"


def scalars_of(doc):
    """(path, value) for scalars a filter operand can name."""
    out = []

    def walk(v, path, depth):
        if isinstance(v, dict):
            for k, c in v.items():
                if SAFE_NAME.match(k) and depth < 4:
                    walk(c, path + "." + k, depth + 1)
        elif isinstance(v, list):
            for i, c in enumerate(v[:3]):
                if depth < 4:
                    walk(c, path + "[%d]" % i, depth + 1)
        else:
            out.append((path, v))

    walk(doc, "$", 0)
    return out


def keys_of(doc):
    out = set()

    def walk(v):
        if isinstance(v, dict):
            for k, c in v.items():
                out.add(k)
                walk(c)
        elif isinstance(v, list):
            for c in v:
                walk(c)

    walk(doc)
    return sorted(out)


def factor(sample, rng, operands):
    ops = ["==", "!=", "<", "<=", ">", ">="]
    op = rng.choice(ops)
    if isinstance(sample, dict) and sample:
        k = rng.choice(list(sample))
        v = sample[k]
        name = member(k, rng) if rng.random() < 0.8 or not SAFE_NAME.match(k) else "." + k
        lhs = "@" + name
        if isinstance(v, list) and v and not isinstance(v[0], (dict, list)):
            if rng.random() < 0.5:
                return "%s[%d]%s%s" % (lhs, rng.randrange(-len(v), len(v)), op, lit(v[0], rng))
            return "%s[?(@%s%s)]" % (lhs, op, lit(rng.choice(v), rng))
        if isinstance(v, (dict, list)) or rng.random() < 0.2:
            return lhs
        value = lit(v, rng)
        if operands and rng.random() < 0.15:
            value = rng.choice(operands)[0]
        if rng.random() < 0.2:
            return "%s%s%s" % (value, op, lhs)
        return "%s%s%s" % (lhs, rng.choice(["", " "]) + op + rng.choice(["", " "]), value)
    if isinstance(sample, (dict, list)):
        return "@.zz"
    if rng.random() < 0.2:
        return "%s%s@" % (lit(sample, rng), op)
    return "@%s%s" % (op, lit(sample, rng))


def filter_expr(node, rng, operands):
    samples = node if isinstance(node, list) and node else [node]
    expr = factor(rng.choice(samples), rng, operands)
    while rng.random() < 0.3:
        nxt = factor(rng.choice(samples), rng, operands)
        expr = "%s%s%s" % (expr, rng.choice([" && ", "&&", " || ", "||"]), nxt)
        if rng.random() < 0.2:
            expr = "(" + expr + ")"
    return "[?(" + expr + ")]"


def random_path(doc, rng, vocab, operands):
    """A path that mostly follows the shape of the document."""
    out = rng.choice(["$", "$", "$", "$", ""])
    cur = [doc]
    for _ in range(rng.randint(1, 5)):
        node = rng.choice(cur) if cur else None
        r = rng.random()
        nxt = []
        if r < 0.08 and vocab:
            what = rng.random()
            if what < 0.6:
                out += ".." + rng.choice(vocab) if SAFE_NAME.match(vocab[0]) else "..*"
            elif what < 0.8:
                out += "..*"
            else:
                out += "..[%d]" % rng.randrange(-2, 3)
            cur = []
            continue
        if isinstance(node, dict) and node:
            ks = list(node)
            if r < 0.6:
                k = rng.choice(ks)
                step = member(k, rng)
                if not out and step.startswith("."):
                    step = rng.choice([step, step[1:]])
                out += step
                nxt = [node[k]]
            elif r < 0.75:
                out += rng.choice([".*", "[*]"])
                nxt = list(node.values())
            elif r < 0.85:
                picks = [rng.choice(ks + ["zz"]) for _ in range(rng.randint(2, 3))]
                out += "[" + ",".join(json.dumps(k) if rng.random() < 0.5 or "'" in k or "\\" in k else "'" + k + "'" for k in picks) + "]"
                nxt = [node[k] for k in picks if k in node]
            elif r < 0.95:
                out += filter_expr(node, rng, operands)
                nxt = [node]
            else:
                out += ".zz"
        elif isinstance(node, list):
            n = len(node)
            if r < 0.3:
                i = rng.randrange(-n - 1, n + 1)
                out += "[%d]" % i
                nxt = [node[i]] if -n <= i < n else []
            elif r < 0.45:
                a, b, c = rng.randrange(-n - 1, n + 2), rng.randrange(-n - 1, n + 2), rng.choice([1, 1, 2, -1, -2, 3])
                out += rng.choice(["[%d:%d]" % (a, b), "[%d:]" % a, "[:%d]" % b, "[%d:%d:%d]" % (a, b, c), "[::%d]" % c])
                nxt = list(node)
            elif r < 0.6:
                out += rng.choice([".*", "[*]"])
                nxt = list(node)
            elif r < 0.7:
                idx = [rng.randrange(-n - 1, n + 1) for _ in range(rng.randint(2, 4))]
                out += "[" + ",".join(map(str, idx)) + "]"
                nxt = [node[i] for i in idx if -n <= i < n]
            elif r < 0.97:
                out += filter_expr(node, rng, operands)
                nxt = list(node)
            else:
                out += ".zz"
        else:
            out += rng.choice([".zz", "[0]", "[*]", ".*", "[?(@==1)]", "[?(@.a)]", "[0:1]", "['a','b']"])
            if node is not None and rng.random() < 0.5:
                out = out[: out.rfind("[") if out.endswith("]") else out.rfind(".")] + filter_expr(node, rng, operands)
                nxt = [node]
        cur = nxt
    return out


# An integer the module would loop on for a very long time, or forever, as a slice step.
RISKY = re.compile(r"\d{9,}")


def mutants(seeds, rng, per_seed):
    alphabet = ".[]()'\"*?@$:, -+0123456789ab=<>!&|\\"
    out = []
    for s in seeds:
        if not s or len(s) > 80:
            continue
        for _ in range(per_seed):
            i = rng.randrange(len(s))
            kind = rng.randrange(5)
            if kind == 0:
                m = s[:i] + s[i + 1 :]
            elif kind == 1:
                m = s[:i] + rng.choice(alphabet) + s[i:]
            elif kind == 2:
                m = s[:i] + rng.choice(alphabet) + s[i + 1 :]
            elif kind == 3:
                m = s[:i] + s[i] + s[i:]
            else:
                j = rng.randrange(len(s))
                m = s[: min(i, j)] + s[max(i, j) :]
            if not RISKY.search(m):
                out.append(m)
    return out


def main():
    sock, test_dir, out_path = sys.argv[1:4]
    r = Resp(sock)
    rng = random.Random(20261114)

    harvested, harvested_docs = harvest(test_dir)
    hand = [p for p in HAND if not RISKY.search(p) or "922337" in p or "184467" in p or "1234567890" in p]
    docs = list(DOCS)
    # A few of the module's own documents, the larger ones first.
    for d in sorted(set(harvested_docs), key=len, reverse=True)[:6]:
        docs.append(d)

    def dedup(seq):
        seen, out = set(), []
        for x in seq:
            if x not in seen:
                seen.add(x)
                out.append(x)
        return out

    hand = dedup(hand)
    harvested = dedup(p for p in harvested if p not in set(hand) and not RISKY.search(p))
    muts = dedup(m for m in mutants(hand + harvested, rng, 2) if m not in set(hand) and m not in set(harvested))

    pairs = []
    for p in hand:
        for d in range(len(DOCS)):
            pairs.append((d, p))
    for p in harvested:
        for d in [0, 1, 4] + list(range(len(DOCS), len(docs))):
            pairs.append((d, p))
    for i, p in enumerate(muts):
        for d in (0, 1, 4, 2 + (i % (len(DOCS) - 2))):
            pairs.append((d, p))
    for p in BUILT:
        for d in (1, 4, 13, 14):
            pairs.append((d, p))
    nrandom = 0
    for d, text in enumerate(docs):
        parsed = json.loads(text)
        if d == 14:
            continue
        vocab, operands = keys_of(parsed), scalars_of(parsed)
        for _ in range(60 if not isinstance(parsed, (dict, list)) or not parsed else 420):
            p = random_path(parsed, rng, vocab, operands)
            if RISKY.search(p):
                continue
            pairs.append((d, p))
            nrandom += 1
            if rng.random() < 0.15:
                pairs.append((rng.randrange(len(docs)), p))
    pairs.extend(big_steps())
    for p in REPEATS:
        for d in range(len(DOCS)):
            pairs.append((d, p))
    pairs = dedup(pairs)

    out = gzip.open(out_path, "wb", compresslevel=9)

    def field(b):
        out.write(b"%d\n" % len(b))
        out.write(b)
        out.write(b"\n")

    canon = []
    for i, d in enumerate(docs):
        reply = r.cmd("JSON.SET", "d%d" % i, ".", d)
        if reply != ("S", b"OK"):
            raise SystemExit("document %d rejected: %r" % (i, reply))
        canon.append(r.cmd("JSON.GET", "d%d" % i, ".")[1])
        out.write(b"D\n")
        field(d.encode())

    def after(key, original):
        now = r.cmd("JSON.GET", key, ".")
        if now[0] == "N":
            return b"!gone"
        return b"=" if now[1] == original else now[1]

    n = probes = 0
    verdicts = {}
    for d, p in pairs:
        key = "d%d" % d
        try:
            get = r.cmd("JSON.GET", key, p)
            r.cmd("JSON.SET", "w", ".", docs[d])
            setr = r.cmd("JSON.SET", "w", p, '"NEW"')
            setdoc = after("w", canon[d])
            r.cmd("JSON.SET", "w", ".", docs[d])
            delr = r.cmd("JSON.DEL", "w", p)
            deldoc = after("w", canon[d])
            if ".." in p:
                arrr, arrdoc = None, b"-"
            else:
                r.cmd("JSON.SET", "w", ".", docs[d])
                arrr = r.cmd("JSON.ARRAPPEND", "w", p, "999")
                arrdoc = after("w", canon[d])
        except (ConnectionError, socket.timeout):
            raise SystemExit("the reference server died or hung on document %d, path %r" % (d, p))
        # How deep a value this path can still take: the module refuses a
        # write when its own path depth plus the depth of the new value passes
        # json.max-path-limit, which exposes that path depth.
        room = b"-"
        if setr == ("S", b"OK") and probes < 9000:
            probes += 1
            lo, hi = 0, 129  # lo is accepted, hi is not
            while hi - lo > 1:
                mid = (lo + hi) // 2
                r.cmd("JSON.SET", "w", ".", docs[d])
                ok = r.cmd("JSON.SET", "w", p, "[" * mid + "]" * mid)
                if ok == ("S", b"OK"):
                    lo = mid
                else:
                    hi = mid
            room = b"%d" % lo
        verdicts.setdefault(p, set()).add(enc(get) if get[0] == "E" else b"ok")
        out.write(b"C %d\n" % d)
        field(p.encode("utf-8", "surrogateescape"))
        field(enc(get))
        field(enc(setr))
        field(setdoc)
        field(enc(delr))
        field(deldoc)
        field(enc(arrr) if arrr else b"-")
        field(arrdoc)
        field(room)
        n += 1
    # The deep documents, with the module's nesting limit for documents lifted.
    r.cmd("CONFIG", "SET", "json.max-path-limit", "1000000")
    ndeep = 0
    for text in DEEP_DOCS:
        d = len(docs)
        docs.append(text)
        if r.cmd("JSON.SET", "deep", ".", text) != ("S", b"OK"):
            raise SystemExit("deep document rejected")
        canon.append(r.cmd("JSON.GET", "deep", ".")[1])
        out.write(b"D\n")
        field(text.encode())
        for p in DEEP_PATHS:
            try:
                get = r.cmd("JSON.GET", "deep", p)
                r.cmd("JSON.SET", "w", ".", text)
                setr = r.cmd("JSON.SET", "w", p, '"NEW"')
                setdoc = after("w", canon[d])
                r.cmd("JSON.SET", "w", ".", text)
                delr = r.cmd("JSON.DEL", "w", p)
                deldoc = after("w", canon[d])
                arrr, arrdoc = None, b"-"
                if ".." not in p:
                    r.cmd("JSON.SET", "w", ".", text)
                    arrr = r.cmd("JSON.ARRAPPEND", "w", p, "999")
                    arrdoc = after("w", canon[d])
            except (ConnectionError, socket.timeout):
                raise SystemExit("the reference server died or hung on deep document %d, path %r" % (d, p[:60]))
            out.write(b"C %d\n" % d)
            for f in (p.encode(), enc(get), enc(setr), setdoc, enc(delr), deldoc, enc(arrr) if arrr else b"-", arrdoc, b"-"):
                field(f)
            n += 1
            ndeep += 1
    out.close()
    print("deep documents: %d, pairs: %d" % (len(DEEP_DOCS), ndeep))
    # For the compile-only test: what JSON.GET said about each hand-written
    # path across all the documents (objects, arrays and scalars). One error
    # for every document means the path is rejected whatever the document is.
    with open(out_path.rsplit("/", 1)[0] + "/static_cases.txt" if "/" in out_path else "static_cases.txt", "wb") as f:
        for p in hand:
            if "\t" in p or "\n" in p or not p:
                continue
            v = verdicts.get(p, set())
            f.write(p.encode() + b"\t" + (next(iter(v))[1:] if len(v) == 1 and b"ok" not in v else b"-") + b"\n")
    print("documents: %d, paths: %d hand + %d harvested + %d mutants + %d built + %d random, pairs: %d, depth probes: %d" %
          (len(docs), len(hand), len(harvested), len(muts), len(BUILT), nrandom, n, probes))


if __name__ == "__main__":
    main()
