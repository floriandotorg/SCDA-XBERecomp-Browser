// The developer console's state, apart from the DOM: the shown lines (bounded), the history Up and Down step through,
// as app/console.cpp keeps them, and what a submitted line sends.
export const CONSOLE_LINES = 200;
export const CONSOLE_HISTORY = 50;

export const createConsoleModel = ({ send }) => {
  const lines = [], history = [];
  let cursor = 0;
  const print = text => {
    for (const line of String(text).split('\n')) { lines.push(line); }
    if (lines.length > CONSOLE_LINES) { lines.splice(0, lines.length - CONSOLE_LINES); }
  };
  return Object.freeze({
    get lines() { return [...lines]; },
    print,
    submit: text => {
      const line = String(text).trim();
      if (!line) { return false; }
      print(`> ${line}`);
      if (history.at(-1) !== line) { history.push(line); }
      if (history.length > CONSOLE_HISTORY) { history.shift(); }
      cursor = history.length;
      if (!send(line)) { print('the game did not take the line'); }
      return true;
    },
    // -1 an older line, 1 a newer one; past the newest an empty line
    recall: step => {
      cursor = Math.max(0, Math.min(history.length, cursor + step));
      return history[cursor] ?? '';
    },
  });
};

// a guest to host message: quit to the main menu, a console output line, or a setting
export const routeHostMessage = (message, { quit, output, setting }, kinds) => {
  if (message.kind === kinds.quit) { quit(); return; }
  if (message.kind === kinds.output) { output(message.text); return; }
  setting(message);
};
