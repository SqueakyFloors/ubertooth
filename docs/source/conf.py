# -- Project information -----------------------------------------------------

project = 'Ubertooth'
copyright = '2021, Great Scott Gadgets'
author = 'Great Scott Gadgets'

# -- General configuration ---------------------------------------------------

extensions = [
    'sphinx.ext.autodoc',
    'sphinx_rtd_theme'
]

templates_path = ['_templates']
source_suffix = '.rst'
master_doc = 'index'
language = 'en'
exclude_patterns = ['_build']
pygments_style = None


# -- Options for HTML output -------------------------------------------------

html_theme = 'sphinx_rtd_theme'
# White mark for the theme's dark sidebar (generated from ../images/ubertooth_logo.png)
html_logo = '../images/ubertooth_mark_dark.png'
html_favicon = '../images/ubertooth_mark.png'
