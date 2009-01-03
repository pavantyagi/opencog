/*
 * MihalceaEdge.cc
 *
 * Implements the edge creation portion of the Rada Mihalcea
 * word-sense disambiguation algorithm. Basically, for every
 * word instance pair, there is a list of associatated senses
 * for each word instance. This class creates edges between
 * each pair of word senses, thus creating a biclique or 
 * complete bipartite graph between the two lists of word senses.
 * 
 * Some word senses may be unrelated, in which case the edge
 * between these to senses is ommited, rather than being assigned
 * a score of zero.
 *
 * Copyright (c) 2008 Linas Vepstas <linas@linas.org>
 */

#include "MihalceaEdge.h"

#include <stdio.h>

#include <opencog/util/platform.h>
#include <opencog/atomspace/SimpleTruthValue.h>
#include <opencog/nlp/wsd/ForeachWord.h>
#include <opencog/nlp/wsd/MihalceaEdge.h>
#include <opencog/nlp/wsd/SenseCache.h>
#include <opencog/nlp/wsd/SenseSimilarityLCH.h>
#include <opencog/nlp/wsd/SenseSimilaritySQL.h>

#define DEBUG
// #define DETAIL_DEBUG
// #define LINK_DEBUG

using namespace opencog;

MihalceaEdge::MihalceaEdge(void)
{
	atom_space = NULL;
#ifdef HAVE_SQL_STORAGE
	sen_sim = new SenseSimilaritySQL();
#else
	fprintf (stderr, 
		"Warning/Error: MihalceaEdge: proper operation of word-sense \n"
		"disambiguation requires precomputed sense similarities to be\n"
		"pulled from SQL stoarage.\n");
	sen_sim = new SenseSimilarityLCH();
#endif /* HAVE_SQL_STORAGE */
}

MihalceaEdge::~MihalceaEdge()
{
	atom_space = NULL;
	delete sen_sim;
}

void MihalceaEdge::set_atom_space(AtomSpace *as)
{
	atom_space = as;
	sense_cache.set_atom_space(as);
}

/** Loop over all parses for this sentence. */
void MihalceaEdge::annotate_sentence(Handle h)
{
	foreach_parse(h, &MihalceaEdge::annotate_parse_f, this);
}

/**
 * For each parse, loop over all word-instance syntactic relationships.
 * (i.e. _subj, _obj, _nn, _amod, and so on). For each relationship,
 * create an edge between all corresponding (word-instance, word-sense)
 * pairs.
 */
void MihalceaEdge::annotate_parse(Handle h)
{
	words.clear();
	foreach_word_instance(h, &EdgeUtils::look_at_word, (EdgeUtils *) this);

	// At this point, "words" contains all of the relex-participating
	// words in the parse. Loop over word-pairs, and annotate them.
	word_pair_count = 0;
	edge_count = 0;
	std::set<Handle>::const_iterator f;
	for (f = words.begin(); f != words.end(); ++f)
	{
		std::set<Handle>::const_iterator s = f;
		s++;
		for (; s != words.end(); ++s)
		{
			annotate_word_pair(*f, *s);
		}
	}
#ifdef DEBUG
	printf("; MihalceaEdge::annotate_parse added %d edges for %d word pairs\n",
		edge_count, word_pair_count);
#endif
}

bool MihalceaEdge::annotate_parse_f(Handle h)
{
	annotate_parse(h);
	return false;
}

/**
 * For each pair of parses, create word-sense edge-links between
 * the two parses.
 */
void MihalceaEdge::annotate_parse_pair(Handle ha, Handle hb)
{
	words.clear();
	foreach_word_instance(ha, &EdgeUtils::look_at_word, (EdgeUtils *) this);
	std::set<Handle> pa_words = words;
	words.clear();
	foreach_word_instance(hb, &EdgeUtils::look_at_word, (EdgeUtils *) this);

#ifdef DETAIL_DEBUG
	printf ("; ========================= start sent pair (%zu x %zu) words\n",
	        pa_words.size(), words.size());
#endif
	// At this point, "pa_words" contains all of the relex-participating
	// words in parse ha, and "words" contains those of parse hb.
	// Loop over word-pairs, and annotate them.
	word_pair_count = 0;
	edge_count = 0;
	std::set<Handle>::const_iterator ia;
	for (ia = pa_words.begin(); ia != pa_words.end(); ia++)
	{
		std::set<Handle>::const_iterator ib;
		for (ib = words.begin(); ib != words.end(); ib++)
		{
			annotate_word_pair(*ia, *ib);
		}
	}
#ifdef DEBUG
	printf ("; annotate_parse_pair: added %d edges for %d word pairs\n",
		edge_count, word_pair_count);
#endif
}

/**
 * Create edges between all senses of a pair of words.
 *
 * This routine implements a doubley-nested foreach loop, iterating
 * over all senses of each word, and creating an edge between them.
 *
 * All of the current word-sense similarity algorithms report zero
 * similarity when the two words are different parts of speech.
 * Therefore, in order to improve performance, this routine does not
 * create any edges between words of differing parts-of-speech.
 */
bool MihalceaEdge::annotate_word_pair(Handle first, Handle second)
{
#ifdef DETAIL_DEBUG
	Node *f = dynamic_cast<Node *>(TLB::getAtom(first));
	Node *s = dynamic_cast<Node *>(TLB::getAtom(second));
	const std::string &fn = f->getName();
	const std::string &sn = s->getName();
	printf ("; WordPair %d: (%s, %s)\n", word_pair_count, fn.c_str(), sn.c_str());
#endif

	second_word_inst = second;
	foreach_word_sense_of_inst(first, &MihalceaEdge::sense_of_first_inst, this);

	word_pair_count ++;
	return false;
}

/**
 * Called for every pair (word-instance,word-sense) of the first
 * word-instance of a relex relationship. This, in turn iterates
 * over the second word-instance of the relex relationship.
 */
bool MihalceaEdge::sense_of_first_inst(Handle first_word_sense_h,
                                       Handle first_sense_link_h)
{
	first_word_sense = first_word_sense_h;

	// printf("first sense %s\n", sense->getName().c_str());
	// Get the handle of the link itself ...
	first_sense_link = first_sense_link_h;

	foreach_word_sense_of_inst(second_word_inst,
	                           &MihalceaEdge::sense_of_second_inst, this);
	return false;
}

/**
 * Use a word-sense similarity/relationship measure to assign an
 * initial truth value to the edge. Create an edge only if the
 * relationship is greater than zero.
 *
 * Called for every pair (word-instance,word-sense) of the second
 * word-instance of a relex relationship. This routine is the last,
 * most deeply nested loop of all of this set of nested loops.  This
 * routine now has possession of both pairs, and can now create a
 * Mihalcea-graph edge between these pairs.
 *
 * As discussed in the README file, the resulting structure is:
 *
 *    <!-- the word "tree" occured in the sentence -->
 *    CosenseLink strength=0.49 confidence=0.3
 *       InheritanceLink strength=0.9 confidence=0.6
 *          WordInstanceNode "tree_99"
 *          WordSenseNode "tree_sense_12"
 *
 *       InheritanceLink strength=0.9 confidence=0.1
 *          WordInstanceNode "bark_144"
 *          WordSenseNode "bark_sense_23"
 */
bool MihalceaEdge::sense_of_second_inst(Handle second_word_sense_h,
                                        Handle second_sense_link)
{
	// printf("second sense %s!\n", sense->getName().c_str());

	// Get the similarity between the two word senses out of the
	// cache (if it exists).
	SimpleTruthValue stv(0.5,0.5);
	stv = sense_cache.similarity(first_word_sense, second_word_sense_h);
	if (stv == TruthValue::DEFAULT_TV())
	{
		// Similarity was not found in the cache. Go fetch a value
		// from the database.
		stv = sen_sim->similarity(first_word_sense, second_word_sense_h);
		sense_cache.set_similarity(first_word_sense, second_word_sense_h, stv);
	}

	// Skip making edges between utterly unrelated nodes.
	if (stv.getMean() < 0.01) return false;

	// Create a link connecting the first pair to the second pair.
	std::vector<Handle> out;
	out.push_back(first_sense_link);
	out.push_back(second_sense_link);

	atom_space->addLink(COSENSE_LINK, out, stv);
	edge_count ++;

#ifdef LINK_DEBUG
	Handle fw = get_word_instance_of_sense_link(first_sense_link);
	Handle fs = get_word_sense_of_sense_link(first_sense_link);
	Node *nfw = dynamic_cast<Node *>(TLB::getAtom(fw));
	Node *nfs = dynamic_cast<Node *>(TLB::getAtom(fs));
	const char *vfw = nfw->getName().c_str();
	const char *vfs = nfs->getName().c_str();

	Handle sw = get_word_instance_of_sense_link(second_sense_link);
	Handle ss = get_word_sense_of_sense_link(second_sense_link);
	Node *nsw = dynamic_cast<Node *>(TLB::getAtom(sw));
	Node *nss = dynamic_cast<Node *>(TLB::getAtom(ss));
	const char *vsw = nsw->getName().c_str();
	const char *vss = nss->getName().c_str();

	printf("slink: %s ## %s <<-->> %s ## %s add\n", vfw, vsw, vfs, vss); 
	printf("slink: %s ## %s <<-->> %s ## %s add\n", vsw, vfw, vss, vfs); 
#endif

	return false;
}
