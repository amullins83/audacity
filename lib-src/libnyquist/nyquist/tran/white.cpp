#include "RngSupport.h"

#include <stdlib.h>

extern "C" {
#include "xlisp.h"
#include "sound.h"

#include "falloc.h"
#include "white.h"

// Undo cext.h madness...
#ifdef min
#undef min
#endif

#ifdef max
#undef max
#endif

#ifdef public
#undef public
#endif

#ifdef private
#undef private
#endif
}

namespace {
class white_susp_node : snd_susp_node
{
private:
   long terminate_cnt;

   Nyq::nyq_uniform_float_distribution distribution{ -1, 1 };
   Nyq::nyq_generator rng;

public:
   white_susp_node() : terminate_cnt(0), rng(Nyq::CreateGenerator())
   { }

   static sound_type snd_make_white(time_type t0, rate_type sr, time_type d);

private:
   void fetch_white(snd_list_type snd_list);

   static void white_free(snd_susp_type a_susp);
   static void white__fetch(snd_susp_type a_susp, snd_list_type snd_list);
};
typedef white_susp_node* white_susp_type;


void white_susp_node::white__fetch(snd_susp_type a_susp, snd_list_type snd_list)
{
   white_susp_type susp = static_cast<white_susp_type>(a_susp);

   susp->fetch_white(snd_list);
}


void white_susp_node::fetch_white(snd_list_type snd_list)
{
   int cnt = 0; /* how many samples computed */
   int togo = 0;
   int n;
   sample_block_type out;
   sample_block_values_type out_ptr;

   sample_block_values_type out_ptr_reg;

   falloc_sample_block(out, "white__fetch");
   out_ptr = out->samples;
   snd_list->block = out;

   while (cnt < max_sample_block_len) { /* outer loop */
  /* first compute how many samples to generate in inner loop: */
  /* don't overflow the output sample block: */
      togo = max_sample_block_len - cnt;

      /* don't run past terminate time */
      if (terminate_cnt != UNKNOWN &&
         terminate_cnt <= current + cnt + togo) {
         togo = terminate_cnt - (current + cnt);
         if (togo < 0) togo = 0;  /* avoids rounding errros */
         if (togo == 0) break;
      }

      n = togo;
      out_ptr_reg = out_ptr;

      if (n) do { /* the inner sample computation loop */
         *out_ptr_reg++ = distribution(rng);
      } while (--n); /* inner loop */

      out_ptr += togo;
      cnt += togo;
   } /* outer loop */

   /* test for termination */
   if (togo == 0 && cnt == 0) {
      snd_list_terminate(snd_list);
   }
   else {
      snd_list->block_len = cnt;
      current += cnt;
   }
} // white_susp_node::fetch_white


inline void ffree_generic_inline(void* sp, size_t nn, const char* who)
{
   // ffree_generic() calls free(), since snd_susp_node has a member 
   // called "free", using the macro from a member function tries to
   // use snd_susp_node.free() instead of ::free().
   ffree_generic(sp, nn, who);
}

void white_susp_node::white_free(snd_susp_type a_susp)
{
   white_susp_type susp = static_cast<white_susp_type>(a_susp);
   susp->~white_susp_node();
   ffree_generic_inline(a_susp, sizeof(white_susp_node), "white_free");
}


void white_print_tree(snd_susp_type a_susp, int n)
{
}


sound_type white_susp_node::snd_make_white(time_type t0, rate_type sr, time_type d)
{
   white_susp_type susp;
   /* sr specified as input parameter */
   /* t0 specified as input parameter */
   sample_type scale_factor = 1.0F;
   falloc_generic(susp, white_susp_node, "snd_make_white");
   memset(static_cast<snd_susp_struct *>(susp), 0, sizeof(snd_susp_struct));
   new (susp) white_susp_node();

   susp->fetch = white__fetch;

   susp->terminate_cnt = check_terminate_cnt(round((d)* sr));
   /* initialize susp state */
   susp->free = white_free;
   susp->sr = sr;
   susp->t0 = t0;
   susp->mark = NULL;
   susp->print_tree = white_print_tree;
   susp->name = "white";
   susp->log_stop_cnt = UNKNOWN;
   susp->current = 0;
   return sound_create(susp, t0, sr, scale_factor);
}
} // anonymous namespace

extern "C" sound_type snd_white(time_type t0, rate_type sr, time_type d)
{
   return white_susp_node::snd_make_white(t0, sr, d);
}
