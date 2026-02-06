/*
 * Safety Tests Module
 *
 * Automated test suite to verify safety fixes on a deployed Move device.
 * Runs all tests in init(), displays results on screen, writes results
 * to test-safety-results.txt for automated collection.
 */

const BASE_DIR = '/data/UserData/move-anything';
const MODULES_DIR = BASE_DIR + '/modules';
const RESULTS_FILE = BASE_DIR + '/test-safety-results.txt';
const TEST_DIR = BASE_DIR + '/test-safety-tmp';

/* Screen constants */
const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;

/* MIDI CCs */
const CC_JOG_WHEEL = 14;
const CC_BACK = 51;

/* Test state */
let results = [];
let passCount = 0;
let failCount = 0;
let scrollOffset = 0;
const VISIBLE_LINES = 4;

/* ===== Test Framework ===== */

function assert(condition, name) {
    if (condition) {
        results.push({ pass: true, name: name });
        passCount++;
    } else {
        results.push({ pass: false, name: name });
        failCount++;
    }
}

function assertThrows(fn, name) {
    try {
        fn();
        /* Did not throw */
        results.push({ pass: false, name: name + ' (no throw)' });
        failCount++;
    } catch (e) {
        results.push({ pass: true, name: name });
        passCount++;
    }
}

function assertNoThrow(fn, name) {
    try {
        fn();
        results.push({ pass: true, name: name });
        passCount++;
    } catch (e) {
        results.push({ pass: false, name: name + ' (threw: ' + e.message + ')' });
        failCount++;
    }
}

/* ===== Test Groups ===== */

function testPathValidation() {
    results.push({ pass: null, name: '--- Path Validation ---' });

    /* Should reject dangerous paths */
    assert(host_file_exists('/etc/passwd') === false,
        'Reject /etc/passwd');

    assert(host_file_exists('/tmp/test') === false,
        'Reject /tmp/test');

    assert(host_file_exists(BASE_DIR + '/../etc/passwd') === false,
        'Reject traversal /../etc/passwd');

    assert(host_file_exists(BASE_DIR + '/modules/../../etc/shadow') === false,
        'Reject deep traversal');

    assert(host_file_exists('') === false,
        'Reject empty string');

    assert(host_file_exists('/') === false,
        'Reject bare /');

    /* Should accept valid paths */
    assert(host_file_exists(MODULES_DIR) === true,
        'Accept modules dir');
}

function testEnsureDir() {
    results.push({ pass: null, name: '--- ensure_dir ---' });

    /* Create a test directory */
    let ok = host_ensure_dir(TEST_DIR);
    assert(ok === true, 'Create dir in BASE_DIR');

    /* Create nested directory */
    ok = host_ensure_dir(TEST_DIR + '/nested/deep');
    assert(ok === true, 'Create nested dir');

    /* Should reject paths outside BASE_DIR */
    ok = host_ensure_dir('/tmp/evil');
    assert(ok === false, 'Reject /tmp/evil');

    ok = host_ensure_dir(BASE_DIR + '/../tmp/evil');
    assert(ok === false, 'Reject traversal dir');
}

function testWriteReadFile() {
    results.push({ pass: null, name: '--- write/read file ---' });

    /* Round-trip basic content */
    let testFile = TEST_DIR + '/test.txt';
    let ok = host_write_file(testFile, 'hello world');
    assert(ok === true, 'Write file');

    let content = host_read_file(testFile);
    assert(content === 'hello world', 'Read file round-trip');

    /* Unicode content */
    let unicodeFile = TEST_DIR + '/unicode.txt';
    ok = host_write_file(unicodeFile, 'hello \u00e9\u00e8\u00ea');
    assert(ok === true, 'Write unicode');

    content = host_read_file(unicodeFile);
    assert(content === 'hello \u00e9\u00e8\u00ea', 'Read unicode round-trip');

    /* Empty string */
    let emptyFile = TEST_DIR + '/empty.txt';
    ok = host_write_file(emptyFile, '');
    assert(ok === true, 'Write empty string');

    content = host_read_file(emptyFile);
    assert(content !== null, 'Read empty file (not null)');

    /* Path validation on write */
    ok = host_write_file('/tmp/evil.txt', 'hack');
    assert(ok === false, 'Write rejects /tmp path');

    /* Path validation on read */
    content = host_read_file('/etc/passwd');
    assert(content === null, 'Read rejects /etc/passwd');
}

function testRemoveDir() {
    results.push({ pass: null, name: '--- remove_dir ---' });

    /* Create and remove a directory inside modules */
    let testModDir = MODULES_DIR + '/test-remove-me';
    host_ensure_dir(testModDir);
    let ok = host_remove_dir(testModDir);
    assert(ok === true, 'Remove valid dir');

    /* Verify removal */
    assert(host_file_exists(testModDir) === false,
        'Dir actually removed');

    /* Should reject dangerous paths */
    ok = host_remove_dir('/tmp');
    assert(ok === false, 'Reject remove /tmp');

    ok = host_remove_dir('/');
    assert(ok === false, 'Reject remove /');

    ok = host_remove_dir(MODULES_DIR + '/../../etc');
    assert(ok === false, 'Reject remove traversal');
}

function testModuleParsing() {
    results.push({ pass: null, name: '--- Module Parsing ---' });

    let modules = host_list_modules();

    assert(Array.isArray(modules), 'list_modules returns array');
    assert(modules.length > 0, 'Has modules');

    /* Check chain module exists */
    let hasChain = false;
    for (let i = 0; i < modules.length; i++) {
        if (modules[i].id === 'chain') hasChain = true;
    }
    assert(hasChain, 'Chain module present');

    /* All modules have id and name */
    let allValid = true;
    for (let i = 0; i < modules.length; i++) {
        if (!modules[i].id || !modules[i].name) {
            allValid = false;
            break;
        }
    }
    assert(allValid, 'All modules have id/name');

    /* Test-safety is visible */
    let hasSelf = false;
    for (let i = 0; i < modules.length; i++) {
        if (modules[i].id === 'test-safety') hasSelf = true;
    }
    assert(hasSelf, 'test-safety visible');
}

function testScreenReader() {
    results.push({ pass: null, name: '--- Screen Reader ---' });

    assertNoThrow(function() {
        host_announce_screenreader('Safety test announcement');
    }, 'screenreader no crash');
}

function testLogging() {
    results.push({ pass: null, name: '--- Logging ---' });

    assertNoThrow(function() {
        console.log('Safety test log message');
    }, 'console.log() works');
}

/* ===== Write Results ===== */

function writeResults() {
    let lines = [];
    lines.push('SAFETY TEST RESULTS');
    lines.push('Pass: ' + passCount + '  Fail: ' + failCount);
    lines.push('Total: ' + (passCount + failCount));
    lines.push('');

    for (let i = 0; i < results.length; i++) {
        let r = results[i];
        if (r.pass === null) {
            lines.push(r.name);
        } else {
            lines.push((r.pass ? 'PASS' : 'FAIL') + ': ' + r.name);
        }
    }

    lines.push('');
    lines.push(failCount === 0 ? 'ALL TESTS PASSED' : 'SOME TESTS FAILED');

    host_write_file(RESULTS_FILE, lines.join('\n'));
}

/* ===== Cleanup ===== */

function cleanup() {
    /* Remove temp test directory */
    host_remove_dir(TEST_DIR);
}

/* ===== UI ===== */

function drawResults() {
    clear_screen();

    /* Header */
    let status = failCount === 0 ? 'ALL PASS' : failCount + ' FAIL';
    print(2, 2, 'Safety: ' + passCount + 'P/' + failCount + 'F', 1);
    fill_rect(0, 12, SCREEN_WIDTH, 1, 1);

    /* Build display lines (filter out section headers for compact view) */
    let displayLines = [];
    for (let i = 0; i < results.length; i++) {
        let r = results[i];
        if (r.pass === null) {
            displayLines.push(r.name);
        } else {
            let prefix = r.pass ? '+' : 'X';
            displayLines.push(prefix + ' ' + r.name);
        }
    }

    /* Show scrollable results */
    let startY = 15;
    let lineH = 11;
    for (let i = 0; i < VISIBLE_LINES && (scrollOffset + i) < displayLines.length; i++) {
        let line = displayLines[scrollOffset + i];
        /* Truncate to fit screen */
        if (line.length > 21) {
            line = line.substring(0, 18) + '...';
        }
        print(2, startY + i * lineH, line, 1);
    }

    /* Scroll indicators */
    if (scrollOffset > 0) {
        print(122, 15, '^', 1);
    }
    if (scrollOffset + VISIBLE_LINES < displayLines.length) {
        print(122, 52, 'v', 1);
    }

    /* Footer */
    fill_rect(0, SCREEN_HEIGHT - 11, SCREEN_WIDTH, 1, 1);
    print(2, SCREEN_HEIGHT - 10, 'Back: exit  Jog: scroll', 1);
}

/* ===== Lifecycle ===== */

globalThis.init = function() {
    results = [];
    passCount = 0;
    failCount = 0;
    scrollOffset = 0;

    /* Run all test groups */
    testPathValidation();
    testEnsureDir();
    testWriteReadFile();
    testRemoveDir();
    testModuleParsing();
    testScreenReader();
    testLogging();

    /* Write results to file for script collection */
    writeResults();

    /* Log summary */
    console.log('Safety Tests: ' + passCount + ' passed, ' + failCount + ' failed');
};

globalThis.tick = function() {
    drawResults();
};

globalThis.onMidiMessageInternal = function(data) {
    let status = data[0] & 0xF0;
    if (status !== 0xB0) return;

    let cc = data[1];
    let val = data[2];

    /* Back button - cleanup and return to menu */
    if (cc === CC_BACK && val > 0) {
        cleanup();
        host_return_to_menu();
        return;
    }

    /* Jog wheel - scroll results */
    if (cc === CC_JOG_WHEEL) {
        let totalLines = results.length;
        if (val > 64) {
            /* Turn right - scroll down */
            if (scrollOffset + VISIBLE_LINES < totalLines) {
                scrollOffset++;
            }
        } else if (val < 64) {
            /* Turn left - scroll up */
            if (scrollOffset > 0) {
                scrollOffset--;
            }
        }
    }
};
