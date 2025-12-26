let line1 = "";
let line2 = "";
let line3 = "";
let line4 = "";
let displayPrev = ["","","",""];

export function updateDisplay() {
    clear_screen();
    print(0, 0, line1, 1);
    print(0, 17, line2, 1);
    print(0, 33, line3, 1);
    print(0, 50, line4, 1);
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
