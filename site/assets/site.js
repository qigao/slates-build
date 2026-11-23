const outline = document.querySelector(".outline");
if (outline) {
  const list = outline.querySelector("ul");
  for (const heading of document.querySelectorAll(".prose h2[id]")) {
    const item = document.createElement("li");
    const link = document.createElement("a");
    link.href = "#" + heading.id;
    link.textContent = heading.textContent;
    item.append(link);
    list.append(item);
  }
  outline.hidden = list.children.length === 0;
}

for (const code of document.querySelectorAll("pre > code")) {
  const block = code.parentElement;
  if (block.scrollWidth > block.clientWidth) {
    block.tabIndex = 0;
  }
  if (block.closest(".terminal") || !navigator.clipboard) continue;

  const panel = document.createElement("div");
  panel.className = "code-panel";
  const tools = document.createElement("div");
  tools.className = "code-tools";
  const button = document.createElement("button");
  button.type = "button";
  button.className = "copy";
  button.textContent = "Copy";
  button.setAttribute("aria-label", "Copy code");
  button.setAttribute("aria-live", "polite");

  let reset;
  button.addEventListener("click", async () => {
    clearTimeout(reset);
    let label;
    try {
      await navigator.clipboard.writeText(code.textContent.trimEnd());
      label = "Copied";
    } catch {
      label = "Copy failed";
    }
    button.textContent = label;
    button.setAttribute("aria-label", label);
    reset = setTimeout(() => {
      button.textContent = "Copy";
      button.setAttribute("aria-label", "Copy code");
    }, 1800);
  });

  block.before(panel);
  tools.append(button);
  panel.append(tools, block);
}
