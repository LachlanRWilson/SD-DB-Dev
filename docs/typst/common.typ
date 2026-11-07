// Shared styling for the UniQOS DB handoff documentation set.

#let accent = rgb("#51247A")
#let rule-colour = rgb("#CFC8D8")
#let mono = ("DejaVu Sans Mono", "Liberation Mono")

#let doc(title, subtitle: none, doc-number: none, body) = {
  set document(title: title)
  set page(
    paper: "a4",
    margin: (x: 2.2cm, y: 2.4cm),
    header: context {
      if here().page() > 1 {
        set text(size: 8pt, fill: luma(90))
        grid(
          columns: (1fr, 1fr),
          align(left)[UniQOS DB -- Handoff Documentation],
          align(right)[#title],
        )
        v(-0.3em)
        line(length: 100%, stroke: 0.5pt + rule-colour)
      }
    },
    footer: context {
      set text(size: 8pt, fill: luma(90))
      line(length: 100%, stroke: 0.5pt + rule-colour)
      v(-0.3em)
      grid(
        columns: (1fr, 1fr),
        align(left)[docs/typst],
        align(right)[#counter(page).display("1 / 1", both: true)],
      )
    },
  )
  set text(font: "New Computer Modern", size: 10.5pt, lang: "en")
  set heading(numbering: "1.1")
  show heading.where(level: 1): it => {
    v(0.6em)
    text(size: 17pt, weight: "bold", fill: accent)[#it.body]
    v(0.2em)
    line(length: 100%, stroke: 1pt + accent)
    v(0.4em)
  }
  show heading.where(level: 2): it => {
    v(0.6em)
    text(size: 13pt, weight: "bold")[#it.body]
    v(0.2em)
  }
  show heading.where(level: 3): it => {
    v(0.4em)
    text(size: 11pt, weight: "bold", style: "italic")[#it.body]
  }
  show raw: it => {
    set text(font: mono, size: 9pt)
    it
  }
  show raw.where(block: true): it => block(
    fill: luma(248),
    stroke: 0.5pt + rule-colour,
    inset: 8pt,
    radius: 3pt,
    width: 100%,
    it,
  )
  show link: it => text(fill: accent, it)

  // Title block
  align(center)[
    #v(1.5cm)
    #text(size: 10pt, fill: luma(100), tracking: 2pt)[
      UniQOS DB -- HANDOFF DOCUMENTATION#if doc-number != none [ -- #doc-number]
    ]
    #v(0.3cm)
    #text(size: 26pt, weight: "bold")[#title]
    #if subtitle != none [
      #v(0.2cm)
      #text(size: 13pt, fill: luma(80))[#subtitle]
    ]
    #v(1cm)
  ]

  body
}

// Status chip for tables, e.g. [stable], [in progress], [open]
#let chip(label, fill, stroke) = box(
  fill: fill,
  stroke: 0.5pt + stroke,
  radius: 2pt,
  inset: (x: 4pt, y: 2pt),
  text(size: 8pt, label),
)

#let stable = chip("stable", rgb("#d5e8d4"), rgb("#82b366"))
#let wip = chip("in progress", rgb("#fff2cc"), rgb("#d6b656"))
#let open-item = chip("open", rgb("#f8cecc"), rgb("#b85450"))

// Figure helper: image + caption, sized to text width
#let fig(path, caption-text, width: 100%) = figure(
  image(path, width: width),
  caption: caption-text,
)

// Callout box for pointers to other docs/notes
#let pointer(body) = block(
  fill: rgb("#f3eef7"),
  stroke: (left: 2.5pt + accent),
  inset: 8pt,
  radius: 2pt,
  width: 100%,
  body,
)
