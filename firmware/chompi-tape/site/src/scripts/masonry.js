// masonry: each card goes into the shortest column, in order
// [container, card selector, narrowest column px, gap px, most columns]
const boxes = [['.overview', ':scope > div', 180, 48, 3]];
for (const [sel, item, min, gap, max] of boxes) for (const box of document.querySelectorAll(sel)) {
  const cards = [...box.querySelectorAll(item)];
  let count = 0;
  const layout = () => {
    const n = Math.min(max, Math.max(1, Math.floor((box.clientWidth + gap) / (min + gap))));
    if (n === count) return;
    count = n;
    const cols = Array.from({length: n}, () => Object.assign(document.createElement('div'), {className: 'col'}));
    box.replaceChildren(...cols);
    box.classList.add('masonry');
    for (const c of cards) cols.reduce((a, b) => b.offsetHeight < a.offsetHeight ? b : a).append(c);
  };
  layout();
  new ResizeObserver(() => layout()).observe(box);
  document.fonts?.ready.then(() => { count = 0; layout(); });
}
