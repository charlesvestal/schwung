/*
 * Shared menu layout helpers for title/list/footer screens.
 */

const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;
const TITLE_Y = 2;
const TITLE_RULE_Y = 12;
const LIST_TOP_Y = 15;
const LIST_LINE_HEIGHT = 11;
const LIST_HIGHLIGHT_HEIGHT = LIST_LINE_HEIGHT + 2;
const LIST_HIGHLIGHT_OFFSET = 0;
const LIST_LABEL_X = 4;
const LIST_VALUE_X = 75;
const LIST_MAX_VISIBLE = 5;
const LIST_INDICATOR_X = 120;
const LIST_INDICATOR_BOTTOM_Y = SCREEN_HEIGHT - 2;
const FOOTER_TEXT_Y = SCREEN_HEIGHT - 11;
const FOOTER_RULE_Y = FOOTER_TEXT_Y - 1;
const LIST_BOTTOM_WITH_FOOTER = FOOTER_RULE_Y - 1;
const DEFAULT_CHAR_WIDTH = 6;
const DEFAULT_LABEL_GAP = 6;
const DEFAULT_VALUE_PADDING_RIGHT = 2;

export function drawMenuHeader(title, titleRight = "") {
    print(2, TITLE_Y, title, 1);

    if (titleRight) {
        const rightX = SCREEN_WIDTH - (titleRight.length * DEFAULT_CHAR_WIDTH) - 2;
        print(Math.max(2, rightX), TITLE_Y, titleRight, 1);
    }

    fill_rect(0, TITLE_RULE_Y, SCREEN_WIDTH, 1, 1);
}

export function drawMenuFooter(text, y = FOOTER_TEXT_Y) {
    if (text) {
        fill_rect(0, FOOTER_RULE_Y, SCREEN_WIDTH, 1, 1);
        print(2, y, text, 1);
    }
}

function drawArrowUp(x, y) {
    set_pixel(x + 2, y, 1);
    set_pixel(x + 1, y + 1, 1);
    set_pixel(x + 3, y + 1, 1);
    for (let i = 0; i < 5; i++) {
        set_pixel(x + i, y + 2, 1);
    }
}

function drawArrowDown(x, y) {
    for (let i = 0; i < 5; i++) {
        set_pixel(x + i, y, 1);
    }
    set_pixel(x + 1, y + 1, 1);
    set_pixel(x + 3, y + 1, 1);
    set_pixel(x + 2, y + 2, 1);
}

export function drawMenuList({
    items,
    selectedIndex,
    listArea,
    topY = LIST_TOP_Y,
    lineHeight = LIST_LINE_HEIGHT,
    highlightHeight = LIST_HIGHLIGHT_HEIGHT,
    highlightOffset = LIST_HIGHLIGHT_OFFSET,
    labelX = LIST_LABEL_X,
    valueX = LIST_VALUE_X,
    valueAlignRight = false,
    valuePaddingRight = DEFAULT_VALUE_PADDING_RIGHT,
    labelGap = DEFAULT_LABEL_GAP,
    maxVisible = 0,
    keepOffLastRow = true,
    indicatorX = LIST_INDICATOR_X,
    indicatorBottomY = LIST_INDICATOR_BOTTOM_Y,
    getLabel,
    getValue
}) {
    const totalItems = items.length;
    const resolvedTopY = listArea?.topY ?? topY;
    const resolvedBottomY = listArea?.bottomY ?? indicatorBottomY;
    const computedMaxVisible = maxVisible > 0
        ? maxVisible
        : Math.max(1, Math.floor((resolvedBottomY - resolvedTopY) / lineHeight));
    const effectiveMaxVisible = computedMaxVisible;
    let startIdx = 0;

    const maxSelectedRow = keepOffLastRow
        ? effectiveMaxVisible - 2
        : effectiveMaxVisible - 1;
    if (selectedIndex > maxSelectedRow) {
        startIdx = selectedIndex - maxSelectedRow;
    }
    let endIdx = Math.min(startIdx + effectiveMaxVisible, totalItems);

    for (let i = startIdx; i < endIdx; i++) {
        const y = resolvedTopY + (i - startIdx) * lineHeight;
        const isSelected = i === selectedIndex;
        const labelPrefix = isSelected ? "> " : "  ";
        let label = getLabel(items[i]);
        const value = getValue ? getValue(items[i]) : "";
        let resolvedValueX = valueX;
        if (valueAlignRight && value) {
            resolvedValueX = SCREEN_WIDTH - (value.length * DEFAULT_CHAR_WIDTH) - valuePaddingRight;
            const maxLabelWidth = Math.max(0, resolvedValueX - labelX - labelGap);
            const maxLabelChars = Math.floor((maxLabelWidth - (labelPrefix.length * DEFAULT_CHAR_WIDTH)) / DEFAULT_CHAR_WIDTH);
            if (maxLabelChars > 0) {
                label = truncateText(label, maxLabelChars);
            }
        }

        if (isSelected) {
            fill_rect(0, y - highlightOffset, SCREEN_WIDTH, highlightHeight, 1);
            print(labelX, y, `${labelPrefix}${label}`, 0);
            if (value) {
                print(resolvedValueX, y, value, 0);
            }
        } else {
            print(labelX, y, `${labelPrefix}${label}`, 1);
            if (value) {
                print(resolvedValueX, y, value, 1);
            }
        }
    }

    if (startIdx > 0) {
        drawArrowUp(indicatorX, resolvedTopY);
    }
    if (endIdx < totalItems) {
        drawArrowDown(indicatorX, resolvedBottomY - 2);
    }
}

export const menuLayoutDefaults = {
    footerY: FOOTER_TEXT_Y,
    listTopY: LIST_TOP_Y,
    listBottomWithFooter: LIST_BOTTOM_WITH_FOOTER,
    listBottomNoFooter: LIST_INDICATOR_BOTTOM_Y
};

function truncateText(text, maxChars) {
    if (text.length <= maxChars) return text;
    if (maxChars <= 3) return text.slice(0, maxChars);
    return `${text.slice(0, maxChars - 3)}...`;
}
