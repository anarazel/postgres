<?xml version='1.0'?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                xmlns:xi="http://www.w3.org/2001/XInclude"
                xmlns="http://www.w3.org/1999/xhtml"
                version='1.0'>

<xsl:import href="http://docbook.sourceforge.net/release/xsl/current/xhtml/docbook.xsl"/>
<xsl:include href="stylesheet-common.xsl" />
<xsl:include href="stylesheet-html-common.xsl" />
<xsl:include href="stylesheet-speedup-xhtml.xsl" />


<xsl:param name="generate.css.header" select="1"/>
<xsl:template name="generate.css.headers">
  <xsl:choose>
    <!-- inline css style sheet -->
    <xsl:when test="$website.stylesheet = 0">
      <style type="text/css">
        <xi:include href="stylesheet.css" parse="text"/>
      </style>
    </xsl:when>
    <!-- link to website -->
    <xsl:otherwise>
      <link rel="stylesheet" type="text/css" href="https://www.postgresql.org/media/css/docs-complete.css"/>
    </xsl:otherwise>
  </xsl:choose>
</xsl:template>


<!-- embed SVG images into output file -->
<xsl:template match="imagedata[@format='SVG']">
  <xsl:variable name="filename">
    <xsl:call-template name="mediaobject.filename">
      <xsl:with-param name="object" select=".."/>
    </xsl:call-template>
  </xsl:variable>

  <xsl:copy-of select="document($filename)"/>
</xsl:template>

</xsl:stylesheet>
