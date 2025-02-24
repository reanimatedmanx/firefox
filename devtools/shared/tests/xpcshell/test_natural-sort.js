/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

const l10n = new Localization(["devtools/client/storage.ftl"], true);
const sessionString = l10n.formatValueSync("storage-expires-session");
const {
  naturalSortCaseSensitive,
  naturalSortCaseInsensitive,
} = require("resource://devtools/shared/natural-sort.js");

function run_test() {
  test("different values types", function () {
    runTest(["a", 1], [1, "a"], "number always comes first");
    runTest(
      ["1", 1],
      ["1", 1],
      "number vs numeric string - should remain unchanged (error in chrome)"
    );
    runTest(
      ["02", 3, 2, "01"],
      ["01", "02", 2, 3],
      "padding numeric string vs number"
    );
  });

  test("datetime", function () {
    runTest(
      ["10/12/2008", "10/11/2008", "10/11/2007", "10/12/2007"],
      ["10/11/2007", "10/12/2007", "10/11/2008", "10/12/2008"],
      "similar dates"
    );
    runTest(
      ["01/01/2008", "01/10/2008", "01/01/1992", "01/01/1991"],
      ["01/01/1991", "01/01/1992", "01/01/2008", "01/10/2008"],
      "similar dates"
    );
    // Years should expand to 0100, 2001, 2010
    runTest(
      ["1/1/100", "1/1/1", "1/1/10"],
      ["1/1/100", "1/1/1", "1/1/10"],
      "dates with short year formatting"
    );
    runTest(
      [
        "Wed Jan 01 2010 00:00:00 GMT-0800 (Pacific Standard Time)",
        "Thu Dec 31 2009 00:00:00 GMT-0800 (Pacific Standard Time)",
        "Wed Jan 01 2010 00:00:00 GMT-0500 (Eastern Standard Time)",
      ],
      [
        "Thu Dec 31 2009 00:00:00 GMT-0800 (Pacific Standard Time)",
        "Wed Jan 01 2010 00:00:00 GMT-0500 (Eastern Standard Time)",
        "Wed Jan 01 2010 00:00:00 GMT-0800 (Pacific Standard Time)",
      ],
      "javascript toString(), different timezones"
    );
    runTest(
      [
        "Saturday, July 3, 2010",
        "Monday, August 2, 2010",
        "Monday, May 3, 2010",
      ],
      [
        "Monday, May 3, 2010",
        "Saturday, July 3, 2010",
        "Monday, August 2, 2010",
      ],
      "Date.toString(), Date.toLocaleString()"
    );
    runTest(
      [
        "Mon, 15 Jun 2009 20:45:30 GMT",
        "Mon, 3 May 2010 17:45:30 GMT",
        "Mon, 15 Jun 2009 17:45:30 GMT",
      ],
      [
        "Mon, 15 Jun 2009 17:45:30 GMT",
        "Mon, 15 Jun 2009 20:45:30 GMT",
        "Mon, 3 May 2010 17:45:30 GMT",
      ],
      "Date.toUTCString()"
    );
    runTest(
      [
        "Saturday, July 3, 2010 1:45 PM",
        "Saturday, July 3, 2010 1:45 AM",
        "Monday, August 2, 2010 1:45 PM",
        "Monday, May 3, 2010 1:45 PM",
      ],
      [
        "Monday, May 3, 2010 1:45 PM",
        "Saturday, July 3, 2010 1:45 AM",
        "Saturday, July 3, 2010 1:45 PM",
        "Monday, August 2, 2010 1:45 PM",
      ],
      ""
    );
    runTest(
      [
        "Saturday, July 3, 2010 1:45:30 PM",
        "Saturday, July 3, 2010 1:45:29 PM",
        "Monday, August 2, 2010 1:45:01 PM",
        "Monday, May 3, 2010 1:45:00 PM",
      ],
      [
        "Monday, May 3, 2010 1:45:00 PM",
        "Saturday, July 3, 2010 1:45:29 PM",
        "Saturday, July 3, 2010 1:45:30 PM",
        "Monday, August 2, 2010 1:45:01 PM",
      ],
      ""
    );
    runTest(
      ["2/15/2009 1:45 PM", "1/15/2009 1:45 PM", "2/15/2009 1:45 AM"],
      ["1/15/2009 1:45 PM", "2/15/2009 1:45 AM", "2/15/2009 1:45 PM"],
      ""
    );
    runTest(
      [
        "2010-06-15T13:45:30",
        "2009-06-15T13:45:30",
        "2009-06-15T01:45:30.2",
        "2009-01-15T01:45:30",
      ],
      [
        "2009-01-15T01:45:30",
        "2009-06-15T01:45:30.2",
        "2009-06-15T13:45:30",
        "2010-06-15T13:45:30",
      ],
      "ISO8601 Dates"
    );
    runTest(
      ["2010-06-15 13:45:30", "2009-06-15 13:45:30", "2009-01-15 01:45:30"],
      ["2009-01-15 01:45:30", "2009-06-15 13:45:30", "2010-06-15 13:45:30"],
      "ISO8601-ish YYYY-MM-DDThh:mm:ss - which does not parse into a Date instance"
    );
    runTest(
      [
        "Mon, 15 Jun 2009 20:45:30 GMT",
        "Mon, 15 Jun 2009 20:45:30 PDT",
        "Mon, 15 Jun 2009 20:45:30 EST",
      ],
      [
        "Mon, 15 Jun 2009 20:45:30 GMT",
        "Mon, 15 Jun 2009 20:45:30 EST",
        "Mon, 15 Jun 2009 20:45:30 PDT",
      ],
      "RFC1123 testing different timezones"
    );
    runTest(
      ["1245098730000", "14330728000", "1245098728000"],
      ["14330728000", "1245098728000", "1245098730000"],
      "unix epoch, Date.getTime()"
    );
    runTest(
      [
        new Date("2001-01-10"),
        "2015-01-01",
        new Date("2001-01-01"),
        "1998-01-01",
      ],
      [
        "1998-01-01",
        new Date("2001-01-01"),
        new Date("2001-01-10"),
        "2015-01-01",
      ],
      "mixed Date types"
    );
    runTest(
      [
        "Tue, 29 Jun 2021 11:31:17 GMT",
        "Sun, 14 Jun 2009 11:11:15 GMT",
        sessionString,
        "Mon, 15 Jun 2009 20:45:30 GMT",
      ],
      [
        sessionString,
        "Sun, 14 Jun 2009 11:11:15 GMT",
        "Mon, 15 Jun 2009 20:45:30 GMT",
        "Tue, 29 Jun 2021 11:31:17 GMT",
      ],
      `"${sessionString}" amongst date strings`
    );
    runTest(
      [
        "Wed, 04 Sep 2024 09:11:44 GMT",
        sessionString,
        "Tue, 06 Sep 2022 09:11:44 GMT",
        sessionString,
        "Mon, 05 Sep 2022 09:12:41 GMT",
      ],
      [
        sessionString,
        sessionString,
        "Mon, 05 Sep 2022 09:12:41 GMT",
        "Tue, 06 Sep 2022 09:11:44 GMT",
        "Wed, 04 Sep 2024 09:11:44 GMT",
      ],
      `"${sessionString}" amongst date strings (complex)`
    );

    runTest(
      [
        "Madras",
        "Jalfrezi",
        "Rogan Josh",
        "Vindaloo",
        "Tikka Masala",
        sessionString,
        "Masala",
        "Korma",
      ],
      [
        "Jalfrezi",
        "Korma",
        "Madras",
        "Masala",
        "Rogan Josh",
        sessionString,
        "Tikka Masala",
        "Vindaloo",
      ],
      `"${sessionString}" amongst strings`
    );
  });

  test("version number strings", function () {
    runTest(
      ["1.0.2", "1.0.1", "1.0.0", "1.0.9"],
      ["1.0.0", "1.0.1", "1.0.2", "1.0.9"],
      "close version numbers"
    );
    runTest(
      ["1.1.100", "1.1.1", "1.1.10", "1.1.54"],
      ["1.1.1", "1.1.10", "1.1.54", "1.1.100"],
      "more version numbers"
    );
    runTest(
      ["1.0.03", "1.0.003", "1.0.002", "1.0.0001"],
      ["1.0.0001", "1.0.002", "1.0.003", "1.0.03"],
      "multi-digit branch release"
    );
    runTest(
      [
        "1.1beta",
        "1.1.2alpha3",
        "1.0.2alpha3",
        "1.0.2alpha1",
        "1.0.1alpha4",
        "2.1.2",
        "2.1.1",
      ],
      [
        "1.0.1alpha4",
        "1.0.2alpha1",
        "1.0.2alpha3",
        "1.1.2alpha3",
        "1.1beta",
        "2.1.1",
        "2.1.2",
      ],
      "close version numbers"
    );
    runTest(
      [
        "myrelease-1.1.3",
        "myrelease-1.2.3",
        "myrelease-1.1.4",
        "myrelease-1.1.1",
        "myrelease-1.0.5",
      ],
      [
        "myrelease-1.0.5",
        "myrelease-1.1.1",
        "myrelease-1.1.3",
        "myrelease-1.1.4",
        "myrelease-1.2.3",
      ],
      "string first"
    );
  });

  test("numerics", function () {
    runTest(["10", 9, 2, "1", "4"], ["1", 2, "4", 9, "10"], "string vs number");
    runTest(
      ["0001", "002", "001"],
      ["0001", "001", "002"],
      "0 left-padded numbers"
    );
    runTest(
      [2, 1, "1", "0001", "002", "02", "001"],
      [1, "1", "0001", "001", 2, "002", "02"],
      "0 left-padded numbers and regular numbers"
    );
    runTest(
      ["10.0401", 10.022, 10.042, "10.021999"],
      ["10.021999", 10.022, "10.0401", 10.042],
      "decimal string vs decimal, different precision"
    );
    runTest(
      ["10.04", 10.02, 10.03, "10.01"],
      ["10.01", 10.02, 10.03, "10.04"],
      "decimal string vs decimal, same precision"
    );
    runTest(
      ["10.04f", "10.039F", "10.038d", "10.037D"],
      ["10.037D", "10.038d", "10.039F", "10.04f"],
      "float/decimal with 'F' or 'D' notation"
    );
    runTest(
      ["10.004Z", "10.039T", "10.038ooo", "10.037g"],
      ["10.004Z", "10.037g", "10.038ooo", "10.039T"],
      "not foat/decimal notation"
    );
    runTest(
      ["1.528535047e5", "1.528535047e7", "1.52e15", "1.528535047e3", "1.59e-3"],
      ["1.59e-3", "1.528535047e3", "1.528535047e5", "1.528535047e7", "1.52e15"],
      "scientific notation"
    );
    runTest(
      ["-1", "-2", "4", "-3", "0", "-5"],
      ["-5", "-3", "-2", "-1", "0", "4"],
      "negative numbers as strings"
    );
    runTest(
      [-1, "-2", 4, -3, "0", "-5"],
      ["-5", -3, "-2", -1, "0", 4],
      "negative numbers as strings - mixed input type, string + numeric"
    );
    runTest(
      [-2.01, -2.1, 4.144, 4.1, -2.001, -5],
      [-5, -2.1, -2.01, -2.001, 4.1, 4.144],
      "negative floats - all numeric"
    );
  });

  test("IP addresses", function () {
    runTest(
      [
        "192.168.0.100",
        "192.168.0.1",
        "192.168.1.1",
        "192.168.0.250",
        "192.168.1.123",
        "10.0.0.2",
        "10.0.0.1",
      ],
      [
        "10.0.0.1",
        "10.0.0.2",
        "192.168.0.1",
        "192.168.0.100",
        "192.168.0.250",
        "192.168.1.1",
        "192.168.1.123",
      ]
    );
  });

  test("filenames", function () {
    runTest(
      ["img12.png", "img10.png", "img2.png", "img1.png"],
      ["img1.png", "img2.png", "img10.png", "img12.png"],
      "simple image filenames"
    );
    runTest(
      [
        "car.mov",
        "01alpha.sgi",
        "001alpha.sgi",
        "my.string_41299.tif",
        "organic2.0001.sgi",
      ],
      [
        "001alpha.sgi",
        "01alpha.sgi",
        "car.mov",
        "my.string_41299.tif",
        "organic2.0001.sgi",
      ],
      "complex filenames"
    );
    runTest(
      [
        "./system/kernel/js/01_ui.core.js",
        "./system/kernel/js/00_jquery-1.3.2.js",
        "./system/kernel/js/02_my.desktop.js",
      ],
      [
        "./system/kernel/js/00_jquery-1.3.2.js",
        "./system/kernel/js/01_ui.core.js",
        "./system/kernel/js/02_my.desktop.js",
      ],
      "unix filenames"
    );
  });

  test("space(s) as first character(s)", function () {
    runTest(["alpha", " 1", "  3", " 2", 0], [0, " 1", " 2", "  3", "alpha"]);
  });

  test("empty strings and space character", function () {
    runTest(
      ["10023", "999", "", 2, 5.663, 5.6629],
      ["", 2, 5.6629, 5.663, "999", "10023"]
    );
    runTest([0, "0", ""], [0, "0", ""]);
  });

  test("hex", function () {
    runTest(["0xA", "0x9", "0x99"], ["0x9", "0xA", "0x99"], "real hex numbers");
    runTest(
      ["0xZZ", "0xVVV", "0xVEV", "0xUU"],
      ["0xUU", "0xVEV", "0xVVV", "0xZZ"],
      "fake hex numbers"
    );
  });

  test("unicode", function () {
    runTest(
      ["\u0044", "\u0055", "\u0054", "\u0043"],
      ["\u0043", "\u0044", "\u0054", "\u0055"],
      "basic latin"
    );
  });

  test("sparse array sort", function () {
    const sarray = [3, 2];
    const sarrayOutput = [1, 2, 3];

    sarray[10] = 1;
    for (let i = 0; i < 8; i++) {
      sarrayOutput.push(undefined);
    }
    runTest(sarray, sarrayOutput, "simple sparse array");
  });

  test("case insensitive support", function () {
    runTest(
      ["A", "b", "C", "d", "E", "f"],
      ["A", "b", "C", "d", "E", "f"],
      "case sensitive pre-sorted array",
      true
    );
    runTest(
      ["A", "C", "E", "b", "d", "f"],
      ["A", "b", "C", "d", "E", "f"],
      "case sensitive un-sorted array",
      true
    );
    runTest(
      ["A", "C", "E", "b", "d", "f"],
      ["A", "C", "E", "b", "d", "f"],
      "case sensitive pre-sorted array"
    );
    runTest(
      ["A", "b", "C", "d", "E", "f"],
      ["A", "C", "E", "b", "d", "f"],
      "case sensitive un-sorted array"
    );
  });

  test("rosetta code natural sort small test set", function () {
    runTest(
      [
        "ignore leading spaces: 2-2",
        " ignore leading spaces: 2-1",
        "  ignore leading spaces: 2+0",
        "   ignore leading spaces: 2+1",
      ],
      [
        "  ignore leading spaces: 2+0",
        "   ignore leading spaces: 2+1",
        " ignore leading spaces: 2-1",
        "ignore leading spaces: 2-2",
      ],
      "Ignoring leading spaces"
    );
    runTest(
      [
        "ignore m.a.s spaces: 2-2",
        "ignore m.a.s  spaces: 2-1",
        "ignore m.a.s   spaces: 2+0",
        "ignore m.a.s    spaces: 2+1",
      ],
      [
        "ignore m.a.s   spaces: 2+0",
        "ignore m.a.s    spaces: 2+1",
        "ignore m.a.s  spaces: 2-1",
        "ignore m.a.s spaces: 2-2",
      ],
      "Ignoring multiple adjacent spaces (m.a.s)"
    );
    runTest(
      [
        "Equiv. spaces: 3-3",
        "Equiv.\rspaces: 3-2",
        "Equiv.\x0cspaces: 3-1",
        "Equiv.\x0bspaces: 3+0",
        "Equiv.\nspaces: 3+1",
        "Equiv.\tspaces: 3+2",
      ],
      [
        "Equiv.\x0bspaces: 3+0",
        "Equiv.\nspaces: 3+1",
        "Equiv.\tspaces: 3+2",
        "Equiv.\x0cspaces: 3-1",
        "Equiv.\rspaces: 3-2",
        "Equiv. spaces: 3-3",
      ],
      "Equivalent whitespace characters"
    );
    runTest(
      [
        "cASE INDEPENENT: 3-2",
        "caSE INDEPENENT: 3-1",
        "casE INDEPENENT: 3+0",
        "case INDEPENENT: 3+1",
      ],
      [
        "casE INDEPENENT: 3+0",
        "case INDEPENENT: 3+1",
        "caSE INDEPENENT: 3-1",
        "cASE INDEPENENT: 3-2",
      ],
      "Case Indepenent sort (naturalSort.insensitive = true)",
      true
    );
    runTest(
      [
        "foo100bar99baz0.txt",
        "foo100bar10baz0.txt",
        "foo1000bar99baz10.txt",
        "foo1000bar99baz9.txt",
      ],
      [
        "foo100bar10baz0.txt",
        "foo100bar99baz0.txt",
        "foo1000bar99baz9.txt",
        "foo1000bar99baz10.txt",
      ],
      "Numeric fields as numerics"
    );
    runTest(
      [
        "The Wind in the Willows",
        "The 40th step more",
        "The 39 steps",
        "Wanda",
      ],
      [
        "The 39 steps",
        "The 40th step more",
        "The Wind in the Willows",
        "Wanda",
      ],
      "Title sorts"
    );
    runTest(
      [
        "Equiv. \xfd accents: 2-2",
        "Equiv. \xdd accents: 2-1",
        "Equiv. y accents: 2+0",
        "Equiv. Y accents: 2+1",
      ],
      [
        "Equiv. y accents: 2+0",
        "Equiv. Y accents: 2+1",
        "Equiv. \xfd accents: 2-2",
        "Equiv. \xdd accents: 2-1",
      ],
      "Equivalent accented characters (and case) (naturalSort.insensitive = true)",
      true
    );
    // This is not a valuable unicode ordering test
    // runTest(
    //   ['Start with an \u0292: 2-2', 'Start with an \u017f: 2-1', 'Start with an \xdf: 2+0', 'Start with an s: 2+1'],
    //   ['Start with an s: 2+1', 'Start with an \xdf: 2+0', 'Start with an \u017f: 2-1', 'Start with an \u0292: 2-2'],
    //   'Character replacements');
  });

  test("contributed tests", function () {
    runTest(
      [
        "T78",
        "U17",
        "U10",
        "U12",
        "U14",
        "745",
        "U7",
        "485",
        "S16",
        "S2",
        "S22",
        "1081",
        "S25",
        "1055",
        "779",
        "776",
        "771",
        "44",
        "4",
        "87",
        "1091",
        "42",
        "480",
        "952",
        "951",
        "756",
        "1000",
        "824",
        "770",
        "666",
        "633",
        "619",
        "1",
        "991",
        "77H",
        "PIER-7",
        "47",
        "29",
        "9",
        "77L",
        "433",
      ],
      [
        "1",
        "4",
        "9",
        "29",
        "42",
        "44",
        "47",
        "77H",
        "77L",
        "87",
        "433",
        "480",
        "485",
        "619",
        "633",
        "666",
        "745",
        "756",
        "770",
        "771",
        "776",
        "779",
        "824",
        "951",
        "952",
        "991",
        "1000",
        "1055",
        "1081",
        "1091",
        "PIER-7",
        "S2",
        "S16",
        "S22",
        "S25",
        "T78",
        "U7",
        "U10",
        "U12",
        "U14",
        "U17",
      ],
      "contributed by Bob Zeiner (Chrome not stable sort)"
    );
    runTest(
      [
        "FSI stop, Position: 5",
        "Mail Group stop, Position: 5",
        "Mail Group stop, Position: 5",
        "FSI stop, Position: 6",
        "FSI stop, Position: 6",
        "Newsstand stop, Position: 4",
        "Newsstand stop, Position: 4",
        "FSI stop, Position: 5",
      ],
      [
        "FSI stop, Position: 5",
        "FSI stop, Position: 5",
        "FSI stop, Position: 6",
        "FSI stop, Position: 6",
        "Mail Group stop, Position: 5",
        "Mail Group stop, Position: 5",
        "Newsstand stop, Position: 4",
        "Newsstand stop, Position: 4",
      ],
      "contributed by Scott"
    );
    runTest(
      [2, 10, 1, "azd", undefined, "asd"],
      [1, 2, 10, "asd", "azd", undefined],
      "issue #2 - undefined support - jarvinen pekka"
    );
    runTest(
      [undefined, undefined, undefined, 1, undefined],
      [1, undefined, undefined, undefined],
      "issue #2 - undefined support - jarvinen pekka"
    );
    runTest(
      ["-1", "-2", "4", "-3", "0", "-5"],
      ["-5", "-3", "-2", "-1", "0", "4"],
      "issue #3 - invalid numeric string sorting - guilermo.dev"
    );
    // native sort implementations are not guaranteed to be stable (i.e. Chrome)
    // runTest(
    //  ['9','11','22','99','A','aaaa','bbbb','Aaaa','aAaa','aa','AA','Aa','aA','BB','bB','aaA','AaA','aaa'],
    //  ['9', '11', '22', '99', 'A', 'aa', 'AA', 'Aa', 'aA', 'aaA', 'AaA', 'aaa', 'aaaa', 'Aaaa', 'aAaa', 'BB', 'bB', 'bbbb'],
    //  'issue #5 - invalid sort order - Howie Schecter (naturalSort.insensitive = true)'m true);
    runTest(
      [
        "9",
        "11",
        "22",
        "99",
        "A",
        "aaaa",
        "bbbb",
        "Aaaa",
        "aAaa",
        "aa",
        "AA",
        "Aa",
        "aA",
        "BB",
        "bB",
        "aaA",
        "AaA",
        "aaa",
      ],
      [
        "9",
        "11",
        "22",
        "99",
        "A",
        "AA",
        "Aa",
        "AaA",
        "Aaaa",
        "BB",
        "aA",
        "aAaa",
        "aa",
        "aaA",
        "aaa",
        "aaaa",
        "bB",
        "bbbb",
      ],
      "issue #5 - invalid sort order - Howie Schecter (naturalSort.insensitive = false)"
    );
    runTest(
      [
        "5D",
        "1A",
        "2D",
        "33A",
        "5E",
        "33K",
        "33D",
        "5S",
        "2C",
        "5C",
        "5F",
        "1D",
        "2M",
      ],
      [
        "1A",
        "1D",
        "2C",
        "2D",
        "2M",
        "5C",
        "5D",
        "5E",
        "5F",
        "5S",
        "33A",
        "33D",
        "33K",
      ],
      "alphanumeric - number first"
    );
    runTest(
      ["img 99", "img199", "imga99", "imgz99"],
      ["img 99", "img199", "imga99", "imgz99"],
      "issue #16 - Sorting incorrect when there is a space - adrien-be"
    );
    runTest(
      ["img199", "img 99", "imga99", "imgz 99", "imgb99", "imgz199"],
      ["img 99", "img199", "imga99", "imgb99", "imgz 99", "imgz199"],
      "issue #16 - expanded test"
    );
    runTest(
      ["1", "02", "3"],
      ["1", "02", "3"],
      "issue #18 - Any zeros that precede a number messes up the sorting - menixator"
    );
    // strings are coerced as floats/ints if possible and sorted accordingly - e.g. they are not chunked
    runTest(
      ["1.100", "1.1", "1.10", "1.54"],
      ["1.100", "1.1", "1.10", "1.54"],
      "issue #13 - ['1.100', '1.10', '1.1', '1.54'] etc do not sort properly... - rubenstolk"
    );
    runTest(
      ["v1.100", "v1.1", "v1.10", "v1.54"],
      ["v1.1", "v1.10", "v1.54", "v1.100"],
      "issue #13 - ['v1.100', 'v1.10', 'v1.1', 'v1.54'] etc do not sort properly... - rubenstolk (bypass float coercion)"
    );
    runTest(
      [
        "MySnmp 1234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567",
        "MySnmp 4234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567",
        "MySnmp 2234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567",
        "MySnmp 3234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567",
      ],
      [
        "MySnmp 1234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567",
        "MySnmp 2234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567",
        "MySnmp 3234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567",
        "MySnmp 4234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567891234567",
      ],
      "issue #14 - Very large numbers make sorting very slow - Mottie"
    );
    runTest(
      ["bar.1-2", "bar.1"],
      ["bar.1", "bar.1-2"],
      "issue #21 - javascript error"
    );
    runTest(
      ["SomeString", "SomeString 1"],
      ["SomeString", "SomeString 1"],
      "PR #19 - ['SomeString', 'SomeString 1'] bombing on 'undefined is not an object' - dannycochran"
    );
    runTest(
      [
        "Udet",
        "\xDCbelacker",
        "Uell",
        "\xDClle",
        "Ueve",
        "\xDCxk\xFCll",
        "Uffenbach",
      ],
      [
        "\xDCbelacker",
        "Udet",
        "Uell",
        "Ueve",
        "Uffenbach",
        "\xDClle",
        "\xDCxk\xFCll",
      ],
      "issue #9 - Sorting umlauts characters \xC4, \xD6, \xDC - diogoalves"
    );
    runTest(
      ["2.2 sec", "1.9 sec", "1.53 sec"],
      ["1.53 sec", "1.9 sec", "2.2 sec"],
      "https://github.com/overset/javascript-natural-sort/issues/13 - ['2.2 sec','1.9 sec','1.53 sec'] - padded by spaces - harisb"
    );
    runTest(
      ["2.2sec", "1.9sec", "1.53sec"],
      ["1.53sec", "1.9sec", "2.2sec"],
      "https://github.com/overset/javascript-natural-sort/issues/13 - ['2.2sec','1.9sec','1.53sec'] - no padding - harisb"
    );
  });
}

function test(description, testFunc) {
  info(description);
  testFunc();
}

function runTest(testArray, expected, description, caseInsensitive = false) {
  let actual = null;

  if (caseInsensitive) {
    actual = testArray.sort((a, b) =>
      naturalSortCaseInsensitive(a, b, sessionString)
    );
  } else {
    actual = testArray.sort((a, b) =>
      naturalSortCaseSensitive(a, b, sessionString)
    );
  }

  compareOptions(actual, expected, description);
}

// deepEqual() doesn't work well for testing arrays containing `undefined` so
// we need to use a custom method.
function compareOptions(actual, expected, description) {
  let match = true;
  for (let i = 0; i < actual.length; i++) {
    if (actual[i] + "" !== expected[i] + "") {
      ok(
        false,
        `${description}\nElement ${i} does not match:\n[${i}] ${actual[i]}\n[${i}] ${expected[i]}`
      );
      match = false;
      break;
    }
  }
  if (match) {
    ok(true, description);
  }
}
