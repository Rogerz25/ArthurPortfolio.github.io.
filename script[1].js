const lenis=new Lenis({autoRaf:true,anchors:true,duration:1.15,smoothWheel:true,syncTouch:false});
const menuBtn=document.querySelector(".menu-btn"),nav=document.querySelector("nav");
menuBtn.addEventListener("click",()=>nav.classList.toggle("open"));
document.querySelectorAll("nav a").forEach(a=>a.addEventListener("click",()=>nav.classList.remove("open")));
const observer=new IntersectionObserver(entries=>entries.forEach(e=>{if(e.isIntersecting){e.target.classList.add("visible");observer.unobserve(e.target)}}),{threshold:.12});
document.querySelectorAll(".reveal").forEach(e=>observer.observe(e));
const moon=document.querySelector(".moon-stage");
if(moon)lenis.on("scroll",({scroll})=>{moon.style.transform=`translate3d(0,${Math.min(scroll*.045,65)}px,0) rotate(${Math.min(scroll*.008,8)}deg)`});
if(window.matchMedia("(prefers-reduced-motion: reduce)").matches)lenis.stop();
