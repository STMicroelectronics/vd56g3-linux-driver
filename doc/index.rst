Fox driver Documentation
========================

Sphinx setup
------------
You should install Sphinx and the associated plugins using the provided requirements.txt file ::

    # inside virtualenv or pew:
    pip install -r requirements.txt


Html version
------------

#. Compile the documentation :: 

    make html

#. Browse the documentation (static way)

    In your preferred web browser, jump to file:./_build/html/index.html

#. Browse the documentation (dynamic way)

    - ``make serve``
    - In your preferred web browser, jump to http://localhost:8000 and browse the doc !!

PDF version
-----------

1. If your system contains the ``latexmk`` utility ::

    make latexpdf

2. If your system contains the ``pdflatex`` utility ::
    
    make latex
    cd _build/latex
    pdflatex kb.tex

   
.. toctree::
    :glob:
    :hidden:
    
    vd56g3_on_rpi
