import { processMidi as octaveUp } from './octave_up.mjs';
import { processMidi as fifths } from './fifths.mjs';

export const midiFxRegistry = {
    octave_up: {
        name: 'octave_up',
        processMidi: octaveUp
    },
    fifths: {
        name: 'fifths',
        processMidi: fifths
    }
};
