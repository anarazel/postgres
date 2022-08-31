<?xml version='1.0'?>

<!--
Processes the entire input document, resolving all entities. Used to transform
postgres.sgml into a single document, for tools that can't deal with multiple
input documents and/or entities.
-->
<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output
      doctype-public="-//OASIS//DTD DocBook XML V4.5//EN"
      doctype-system="http://www.oasis-open.org/docbook/xml/4.5/docbookx.dtd"/>
  <xsl:template match="@*|node()">
    <xsl:copy>
      <xsl:apply-templates select="@*|node()"/>
    </xsl:copy>
  </xsl:template>
</xsl:stylesheet>
