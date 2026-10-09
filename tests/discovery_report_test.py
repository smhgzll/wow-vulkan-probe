#!/usr/bin/env python3
"""Regression tests for conservative producer/consumer and comparison evidence."""
import copy
import importlib.util
from pathlib import Path
import unittest

path=Path(__file__).resolve().parents[1]/"tools/discovery-report.py"
spec=importlib.util.spec_from_file_location("discovery_report",path)
module=importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def fixture():
    view=dict(image="0000000000000010",imageGeneration=1,aspect=2,baseMip=0,mipCount=1,baseLayer=0,layerCount=1,
              mips=1,layers=1,format=126,width=2048,height=2048)
    target=dict(known=1,depth=view,colors=[],role=3)
    producer=dict(pipeline=1,skipped=0,target=target,depthTest=1,depthWrite=1,rasterDiscard=0,unsupportedState=0,
                  descriptorsIncomplete=0,resources=[],kind=0,blend=0)
    resource=dict(view=copy.deepcopy(view),type=1,known=1,set=0,binding=2,staticDref=1,staticSample=1,updateAfterBind=0,
                  sampler=dict(compareEnable=1,minFilter=1,magFilter=1))
    consumer=dict(pipeline=2,skipped=0,target=dict(known=1,depth=None,colors=[],role=2),depthTest=1,depthWrite=0,
                  rasterDiscard=0,unsupportedState=0,descriptorsIncomplete=0,resources=[resource],kind=1,blend=0)
    pipelines=[dict(id=i,hash=f"{i:016x}",vs="a"*16,fs="b"*16,blend=0) for i in (1,2)]
    return dict(pipelines=pipelines,shaders=[],frames=[dict(number=1,truncated=0,missingCommands=0,draws=[producer,consumer])],
                _path="synthetic.json",_sha256="0"*64)


class EvidenceTests(unittest.TestCase):
    def test_ordered_binding_is_candidate_only(self):
        capture=fixture()
        edges=module.depth_links(capture)
        self.assertEqual(len(edges),1)
        self.assertTrue(edges[0]["dref"] and edges[0]["compare"])
        text=module.report(capture)
        self.assertIn("shadow candidates, not confirmed",text)
        self.assertIn("Directional light: unknown",text)
        self.assertIn("Confirmed pipelines:\n- None",text)

    def test_no_link_before_producer_or_for_reused_handle(self):
        capture=fixture();capture["frames"][0]["draws"].reverse()
        self.assertEqual(module.depth_links(capture),[])
        capture=fixture();capture["frames"][0]["draws"][1]["resources"][0]["view"]["imageGeneration"]=2
        self.assertEqual(module.depth_links(capture),[])

    def test_subresource_and_skipped_draws(self):
        capture=fixture();capture["frames"][0]["draws"][1]["resources"][0]["view"]["baseLayer"]=1
        self.assertEqual(module.depth_links(capture),[])
        capture=fixture();capture["frames"][0]["draws"][0]["skipped"]=1
        self.assertEqual(module.depth_links(capture),[])

    def test_uncertainty_survives_uab_and_truncation(self):
        capture=fixture();capture["frames"][0]["truncated"]=1
        capture["frames"][0]["draws"][1]["resources"][0]["updateAfterBind"]=1
        edge=module.depth_links(capture)[0]
        self.assertTrue(edge["uab"] and edge["uncertain"])

    def test_water_comparison_does_not_assign_semantics(self):
        visible=fixture();control=fixture();control["frames"][0]["draws"].pop()
        text=module.report(visible,control)
        self.assertIn("+1.00",text)
        self.assertIn("none is automatically classified as water",text)
        self.assertIn("semantic meaning unconfirmed",text)

    def test_same_count_with_changed_dynamic_state_or_texture(self):
        visible=fixture();control=fixture()
        control["frames"][0]["draws"][1]["blend"]=1
        control["frames"][0]["draws"][1]["resources"][0]["view"]["width"]=1024
        text=module.report(visible,control)
        self.assertIn("blends, textures",text)
        self.assertIn("+0.00",text)
        self.assertEqual(sum(module.transitions(visible).values()),1)


if __name__=="__main__":unittest.main()
