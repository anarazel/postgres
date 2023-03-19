<?xml version="1.0" encoding="utf-8"?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                version='1.0'>

<!-- Performance-optimized versions of some upstream templates from common/
     directory -->

<!-- from common/labels.xsl -->

<xsl:template match="chapter" mode="label.markup">
  <xsl:choose>
    <xsl:when test="@label">
      <xsl:value-of select="@label"/>
    </xsl:when>
    <xsl:when test="string($chapter.autolabel) != 0">
      <xsl:if test="$component.label.includes.part.label != 0 and
                      ancestor::part">
        <xsl:variable name="part.label">
          <xsl:apply-templates select="ancestor::part"
                               mode="label.markup"/>
        </xsl:variable>
        <xsl:if test="$part.label != ''">
          <xsl:value-of select="$part.label"/>
          <xsl:apply-templates select="ancestor::part"
                               mode="intralabel.punctuation">
            <xsl:with-param name="object" select="."/>
          </xsl:apply-templates>
        </xsl:if>
      </xsl:if>
      <xsl:variable name="format">
        <xsl:call-template name="autolabel.format">
          <xsl:with-param name="format" select="$chapter.autolabel"/>
        </xsl:call-template>
      </xsl:variable>
      <xsl:choose>
        <xsl:when test="$label.from.part != 0 and ancestor::part">
          <xsl:number from="part" count="chapter" format="{$format}" level="any"/>
        </xsl:when>
        <xsl:otherwise>
          <!-- Optimization for pgsql-docs: When counting to get label for
               this chapter, preceding chapters can only be our siblings or
               children of a preceding part, so only count those instead of
               scanning the entire node tree. -->
          <!-- <xsl:number from="book" count="chapter" format="{$format}" level="any"/> -->
          <xsl:number value="count(../preceding-sibling::part/chapter) + count(preceding-sibling::chapter) + 1" format="{$format}"/>
        </xsl:otherwise>
      </xsl:choose>
    </xsl:when>
  </xsl:choose>
</xsl:template>

<xsl:template match="appendix" mode="label.markup">
  <xsl:choose>
    <xsl:when test="@label">
      <xsl:value-of select="@label"/>
    </xsl:when>
    <xsl:when test="string($appendix.autolabel) != 0">
      <xsl:if test="$component.label.includes.part.label != 0 and
                      ancestor::part">
        <xsl:variable name="part.label">
          <xsl:apply-templates select="ancestor::part"
                               mode="label.markup"/>
        </xsl:variable>
        <xsl:if test="$part.label != ''">
          <xsl:value-of select="$part.label"/>
          <xsl:apply-templates select="ancestor::part"
                               mode="intralabel.punctuation">
            <xsl:with-param name="object" select="."/>
          </xsl:apply-templates>
        </xsl:if>
      </xsl:if>
      <xsl:variable name="format">
        <xsl:call-template name="autolabel.format">
          <xsl:with-param name="format" select="$appendix.autolabel"/>
        </xsl:call-template>
      </xsl:variable>
      <xsl:choose>
        <xsl:when test="$label.from.part != 0 and ancestor::part">
          <xsl:number from="part" count="appendix" format="{$format}" level="any"/>
        </xsl:when>
        <xsl:otherwise>
          <!-- Optimization for pgsql-docs: When counting to get label for
               this appendix, preceding appendixes can only be our siblings or
               children of a preceding part, so only count those instead of
               scanning the entire node tree. -->
          <!-- <xsl:number from="book|article" count="appendix" format="{$format}" level="any"/> -->
          <xsl:number value="count(../preceding-sibling::part/appendix) + count(preceding-sibling::appendix) + 1" format="{$format}"/>
        </xsl:otherwise>
      </xsl:choose>
    </xsl:when>
  </xsl:choose>
</xsl:template>

<!-- from common/l10n.xsl -->

<!-- Just hardcode the language for the whole document, to make it faster. -->

<xsl:template name="l10n.language">en</xsl:template>
<xsl:param name='pg.l10n.xml' select="document('file:///usr/share/xml/docbook/stylesheet/docbook-xsl/common/en.xml')[1]"/>

<xsl:template name="gentext.template.recurse">
  <xsl:param name="context"/>
  <xsl:param name="name"/>
  <xsl:param name="origname"/>
  <xsl:param name="verbose"/>


  <xsl:choose>
    <xsl:when test="contains($name, '/')">
      <xsl:call-template name="gentext.template.recurse">
        <xsl:with-param name="context" select="$context"/>
        <xsl:with-param name="name" select="substring-after($name, '/')"/>
        <xsl:with-param name="origname" select="$origname"/>
        <xsl:with-param name="verbose" select="$verbose"/>
      </xsl:call-template>
    </xsl:when>
    <xsl:otherwise>

      <!-- FIXME: should find a way to avoid the concat and [1] here -->
      <xsl:variable name="template.node"
                    select="key('l10n-template', concat($context, '#', $name))[1]"/>

      <xsl:choose>
        <xsl:when test="$template.node/@text">
          <xsl:value-of select="$template.node/@text"/>
        </xsl:when>
        <xsl:when test="$verbose = 0">
        </xsl:when>
        <xsl:otherwise>
          <xsl:message>
            <xsl:text>No template for "</xsl:text>
            <xsl:value-of select="$origname"/>
            <xsl:text>" (or any of its leaves) exists in the context named "</xsl:text>
            <xsl:value-of select="$context"/>
            <xsl:text>" in the "</xsl:text>
            <xsl:text>" en localization.</xsl:text>
          </xsl:message>
        </xsl:otherwise>
      </xsl:choose>
    </xsl:otherwise>
  </xsl:choose>
</xsl:template>


<xsl:template name="gentext.template">
  <xsl:param name="context" select="'default'"/>
  <xsl:param name="name" select="'default'"/>
  <xsl:param name="origname" select="$name"/>
  <xsl:param name="lang" select="'en'"/>
  <xsl:param name="verbose" select="1"/>

  <!-- FIXME: unnecessary recursion for leading -->
  <xsl:for-each select="$pg.l10n.xml">
    <xsl:variable name="context.node"
                  select="key('l10n-context', $context)[1]"/>

    <xsl:if test="count($context.node) = 0
                  and $verbose != 0">
      <xsl:message>
        <xsl:text>No context named "</xsl:text>
        <xsl:value-of select="$context"/>
        <xsl:text>" exists in the "</xsl:text>
        <xsl:value-of select="$lang"/>
        <xsl:text>" localization.</xsl:text>
      </xsl:message>
    </xsl:if>

    <xsl:for-each select="$context.node">
      <xsl:call-template name="gentext.template.recurse">
        <xsl:with-param name="context" select="$context"/>
        <xsl:with-param name="name" select="$name"/>
        <xsl:with-param name="origname" select="$origname"/>
        <xsl:with-param name="verbose" select="$verbose"/>
      </xsl:call-template>
    </xsl:for-each>
  </xsl:for-each>
</xsl:template>

</xsl:stylesheet>
