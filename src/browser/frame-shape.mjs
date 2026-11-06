// The frame's shape on the page: Auto follows the window (4:3 at the narrowest, as native's Aspect), a fixed ratio
// keeps its shape, and the CSS box fits, steps in whole multiples of 480 device rows, or stretches to the window.
export const ASPECTS = Object.freeze({ '4:3': 4 / 3, '16:10': 16 / 10, '16:9': 16 / 9, '21:9': 21 / 9 });
export const FRAME_HEIGHT = 480;

export const frameShape = ({ aspect = 'auto', scaling = 'fit', width, height, ratio = 1 }) => {
  const effective = ASPECTS[aspect] ?? Math.max(4 / 3, width / height);
  if (scaling === 'stretch') { return { aspect: effective, width, height }; }
  if (scaling === 'integer') {
    const rows = Math.min(Math.floor(height * ratio / FRAME_HEIGHT), Math.floor(width * ratio / (FRAME_HEIGHT * effective)));
    if (rows >= 1) { return { aspect: effective, width: rows * FRAME_HEIGHT * effective / ratio, height: rows * FRAME_HEIGHT / ratio }; }
  }
  return width / height > effective ? { aspect: effective, width: height * effective, height } : { aspect: effective, width, height: width / effective };
};

// the canvas in pixels for a frame of that shape: as wide as the shape at the frame's height
export const canvasSize = (aspect, height = FRAME_HEIGHT) => ({ width: Math.round(height * aspect), height });
