# Configuration file for the Sphinx documentation builder.
#
# This file only contains a selection of the most common options. For a full
# list see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

# -- Path setup --------------------------------------------------------------

# If extensions (or modules to document with autodoc) are in another directory,
# add these directories to sys.path here. If the directory is relative to the
# documentation root, use os.path.abspath to make it absolute, like shown here.
#
# import os
# import sys
# sys.path.insert(0, os.path.abspath('.'))


# -- Project information -----------------------------------------------------

project = 'VD56G3 Driver'
copyright = '2021-23, ST Microelectronics'
author = 'SP'

# The full version, including alpha/beta/rc tags
release = 'doc-0.4'


# -- General configuration ---------------------------------------------------

# Temporary: in sphinx>2, master_doc default to index
master_doc = 'index'

# Add any Sphinx extension module names here, as strings. They can be
# extensions coming with Sphinx (named 'sphinx.ext.*') or your custom
# ones.
extensions = ['sphinx.ext.graphviz',        # Sphinx's Graphviz builtin extension
              'linuxdoc.rstKernelDoc',      # Linuxdoc extension: 'kernel-doc' reST-directive.
              'linuxdoc.kernel_include',    # Linuxdoc extension: 'kernel-include' reST-directive.
              'linuxdoc.cdomain']           # Linuxdoc extension: Replacement for the sphinx c-domain.

# Add any paths that contain templates here, relative to this directory.
templates_path = ['_templates']

# List of patterns, relative to source directory, that match files and
# directories to ignore when looking for source files.
# This pattern also affects html_static_path and html_extra_path.
exclude_patterns = ['_build', 'venv', 'Thumbs.db', '.DS_Store']


# -- Options for HTML output -------------------------------------------------

# The theme to use for HTML and HTML Help pages.  See the documentation for
# a list of builtin themes.
#
html_theme = 'sphinx_rtd_theme'

# Theme options are theme-specific and customize the look and feel of a theme
# further.  For a list of options available for each theme, see the
# documentation.
html_theme_options = {
    "collapse_navigation" : False,
#    'titles_only': True
}


# Add any paths that contain custom static files (such as style sheets) here,
# relative to this directory. They are copied after the builtin static files,
# so a file named "default.css" will overwrite the builtin "default.css".
html_static_path = ['_sphinx_static']


# -- Customization  ----------------------------------------------------------
def setup(app):
    app.add_css_file('rtd_theme_overrides.css')

rst_prolog = """
.. role:: reqtracer
"""

# -- Options for LaTeX output ---------------------------------------------

latex_engine = 'pdflatex'

latex_elements = {
    'releasename': '\\mbox{}',
    'preamble': r'''
\usepackage{draftwatermark}
\SetWatermarkText{ST CONFIDENTIAL}
\SetWatermarkFontSize{1.5cm}
\SetWatermarkColor[rgb]{1,0.7,0.7}
    ''',
    'figure_align': 'H',
}

# List of tuples (startdocname, targetname, title, author, theme, toctree_only)
latex_documents = [('vd56g3','vd56g3.tex','VD56G3 Linux Driver Documentation','','manual')]

latex_logo = '_sphinx_static/logo.png'

