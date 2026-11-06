/* cases for tests/js_test.c: check(source, expected string of the last expression) */
check("1+2", "3");
check("'a'+'b'", "ab");
check("var x = 5; x * 2", "10");
check("typeof undefined_thing", "undefined");
check("[1,2,3].map(x=>x*2).join('-')", "2-4-6");
