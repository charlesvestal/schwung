let line1 = "";
let line2 = "";
let line3 = "";
let line4 = "";
let overlay = [" "," "," "," "];
let displayPrev = ["","","",""];

export function updateDisplay(clear=true) {
    if (clear) {
        clear_screen();
    }
    print(0, 0, line1, 1);
    print(0, 17, line2, 1);
    print(0, 33, line3, 1);
    print(0, 50, line4, 1);
}

export function updateOverlay(position=0) {
    if (position === 0) {
        overlay = ["<","","",""]
    } else if (position === 1) {
        overlay = ["","<","",""]
    } else if (position === 2) {
        overlay = ["","","<",""]
    } else if (position === 3) {
        overlay = ["","","","<"]
    }
    print(120, 0, overlay[0], 1);
    print(120, 17, overlay[1], 1);
    print(120, 33, overlay[2], 1);
    print(120, 50, overlay[3], 1);
}

export function display(l1=" ", l2=" ", l3=" ", l4=" ", temp=false, prev=false) {
    if (prev) {
        line1 = displayPrev[0];
        line2 = displayPrev[1];
        line3 = displayPrev[2];
        line4 = displayPrev[3];
        return;
    }

    line1 = l1;
    line2 = l2;
    line3 = l3;
    line4 = l4;
    if (!temp) {
        displayPrev = [l1, l2, l3, l4];
    }
}
