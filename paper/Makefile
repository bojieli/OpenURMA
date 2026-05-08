# SPDX-License-Identifier: CC-BY-4.0
DOC = main
LATEX = pdflatex
BIBTEX = bibtex

all: $(DOC).pdf

$(DOC).pdf: $(DOC).tex references.bib
	$(LATEX) -interaction=nonstopmode $(DOC) || true
	$(BIBTEX) $(DOC) || true
	$(LATEX) -interaction=nonstopmode $(DOC) || true
	$(LATEX) -interaction=nonstopmode $(DOC) || true

clean:
	rm -f $(DOC).aux $(DOC).bbl $(DOC).blg $(DOC).log $(DOC).out $(DOC).toc

distclean: clean
	rm -f $(DOC).pdf

.PHONY: all clean distclean
