"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
const ansiStyles = require("ansi-styles");
const chalk = require("chalk");
const supports = require("supports-color");
const util_1 = require("util");
const stripColor = (s) => {
    return require('strip-ansi')(s);
};
const dim = process.env.ConEmuANSI === 'ON' ? chalk.gray : chalk.dim;
exports.CustomColors = {
    supports,
    // map gray -> dim because it's not solarized compatible
    gray: dim,
    grey: dim,
    dim,
    attachment: chalk.cyan,
    addon: chalk.yellow,
    configVar: chalk.green,
    release: chalk.blue.bold,
    cmd: chalk.cyan.bold,
    pipeline: chalk.green.bold,
    // eslint-disable-next-line @typescript-eslint/no-use-before-define
    app: (s) => chalk.level > 0 ? exports.color.heroku(`⬢ ${s}`) : s,
    heroku: (s) => {
        if (chalk.level === 0)
            return s;
        // eslint-disable-next-line @typescript-eslint/no-use-before-define
        if (!exports.color.supports)
            return s;
        // eslint-disable-next-line @typescript-eslint/no-use-before-define
        const has256 = exports.color.level >= 2 || (process.env.TERM || '').indexOf('256') !== -1;
        return has256 ? '\u001b[38;5;104m' + s + ansiStyles.reset.open : chalk.magenta(s); // eslint-disable-line unicorn/escape-case
    },
    stripColor: util_1.deprecate(stripColor, '.stripColor is deprecated. Please import the "strip-ansi" module directly instead.'),
};
exports.color = new Proxy(chalk, {
    get: (chalk, name) => {
        if (exports.CustomColors[name])
            return exports.CustomColors[name];
        return chalk[name];
    },
    set: (chalk, name, value) => {
        switch (name) {
            case 'enabled':
                if (!value)
                    chalk.level = 0;
                break;
            default:
                throw new Error(`cannot set property ${name.toString()}`);
        }
        return true;
    },
});
exports.default = exports.color;
